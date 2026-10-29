#include "goober/webpush/webpush.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/obj_mac.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>

#include <memory>

#include "goober/base/hash.h"

namespace goober::webpush {

namespace {

template <typename T, void (*F)(T*)>
struct Deleter {
  void operator()(T* p) const { F(p); }
};
using Pkey = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY, EVP_PKEY_free>>;
using PkeyCtx = std::unique_ptr<EVP_PKEY_CTX, Deleter<EVP_PKEY_CTX, EVP_PKEY_CTX_free>>;
using Group = std::unique_ptr<EC_GROUP, Deleter<EC_GROUP, EC_GROUP_free>>;
using Point = std::unique_ptr<EC_POINT, Deleter<EC_POINT, EC_POINT_free>>;
using Bn = std::unique_ptr<BIGNUM, Deleter<BIGNUM, BN_free>>;
using ParamBld = std::unique_ptr<OSSL_PARAM_BLD, Deleter<OSSL_PARAM_BLD, OSSL_PARAM_BLD_free>>;
using Params = std::unique_ptr<OSSL_PARAM, Deleter<OSSL_PARAM, OSSL_PARAM_free>>;
using MdCtx = std::unique_ptr<EVP_MD_CTX, Deleter<EVP_MD_CTX, EVP_MD_CTX_free>>;
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, Deleter<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free>>;
using Sig = std::unique_ptr<ECDSA_SIG, Deleter<ECDSA_SIG, ECDSA_SIG_free>>;

constexpr size_t kRecordSize = 4096;

bool fail(std::string* error, std::string what) {
  unsigned long e = ERR_get_error();
  if (e) {
    char buf[256];
    ERR_error_string_n(e, buf, sizeof buf);
    what += ": ";
    what += buf;
  }
  ERR_clear_error();
  if (error) *error = std::move(what);
  return false;
}

std::string hmac(std::string_view key, std::string_view data) {
  auto h = base::hmac_sha256(key, data);
  return std::string(reinterpret_cast<const char*>(h.data()), h.size());
}

// An EVP_PKEY for a P-256 public point, or with the private scalar too.
Pkey make_key(std::string_view public_raw, std::string_view private_raw, std::string* error) {
  ParamBld bld(OSSL_PARAM_BLD_new());
  Bn priv;
  if (!bld || !OSSL_PARAM_BLD_push_utf8_string(bld.get(), OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) ||
      !OSSL_PARAM_BLD_push_octet_string(bld.get(), OSSL_PKEY_PARAM_PUB_KEY, public_raw.data(), public_raw.size())) {
    fail(error, "key parameters");
    return nullptr;
  }
  if (!private_raw.empty()) {
    priv.reset(BN_bin2bn(reinterpret_cast<const unsigned char*>(private_raw.data()), int(private_raw.size()), nullptr));
    if (!priv || !OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_PRIV_KEY, priv.get())) {
      fail(error, "private key parameter");
      return nullptr;
    }
  }
  Params params(OSSL_PARAM_BLD_to_param(bld.get()));
  PkeyCtx ctx(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
  EVP_PKEY* raw = nullptr;
  if (!params || !ctx || EVP_PKEY_fromdata_init(ctx.get()) <= 0 ||
      EVP_PKEY_fromdata(ctx.get(), &raw, private_raw.empty() ? EVP_PKEY_PUBLIC_KEY : EVP_PKEY_KEYPAIR, params.get()) <= 0) {
    fail(error, "not a P-256 key");
    return nullptr;
  }
  return Pkey(raw);
}

std::optional<std::string> ecdh(const Key& mine, std::string_view peer_public, std::string* error) {
  auto me = make_key(mine.public_raw, mine.private_raw, error);
  auto peer = make_key(peer_public, "", error);
  if (!me || !peer) return std::nullopt;
  PkeyCtx ctx(EVP_PKEY_CTX_new(me.get(), nullptr));
  size_t len = 0;
  if (!ctx || EVP_PKEY_derive_init(ctx.get()) <= 0 || EVP_PKEY_derive_set_peer(ctx.get(), peer.get()) <= 0 ||
      EVP_PKEY_derive(ctx.get(), nullptr, &len) <= 0) {
    fail(error, "ECDH");
    return std::nullopt;
  }
  std::string out(len, '\0');
  if (EVP_PKEY_derive(ctx.get(), reinterpret_cast<unsigned char*>(out.data()), &len) <= 0) {
    fail(error, "ECDH");
    return std::nullopt;
  }
  out.resize(len);
  return out;
}

// The content key and nonce (RFC 8291 § 3.4, RFC 8188 § 2.2).
struct Keys {
  std::string cek, nonce;
};
Keys derive(std::string_view ecdh_secret, std::string_view auth_secret, std::string_view ua_public,
            std::string_view as_public, std::string_view salt) {
  std::string key_info = std::string("WebPush: info") + '\0' + std::string(ua_public) + std::string(as_public);
  std::string prk_key = hmac(auth_secret, ecdh_secret);
  std::string ikm = hmac(prk_key, key_info + '\x01');
  std::string prk = hmac(salt, ikm);
  Keys k;
  k.cek = hmac(prk, std::string("Content-Encoding: aes128gcm") + '\0' + '\x01').substr(0, 16);
  k.nonce = hmac(prk, std::string("Content-Encoding: nonce") + '\0' + '\x01').substr(0, 12);
  return k;
}

std::optional<std::string> gcm(bool seal, std::string_view key, std::string_view nonce, std::string_view in,
                               std::string* error) {
  CipherCtx ctx(EVP_CIPHER_CTX_new());
  auto u = [](std::string_view s) { return reinterpret_cast<const unsigned char*>(s.data()); };
  if (!ctx || !(seal ? EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_gcm(), nullptr, u(key), u(nonce))
                     : EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_gcm(), nullptr, u(key), u(nonce)))) {
    fail(error, "AES-128-GCM");
    return std::nullopt;
  }
  if (!seal && in.size() < 16) {
    fail(error, "the record is too short");
    return std::nullopt;
  }
  std::string_view body = seal ? in : in.substr(0, in.size() - 16);
  std::string out(body.size() + 16, '\0');
  int len = 0, fin = 0;
  auto o = reinterpret_cast<unsigned char*>(out.data());
  if (seal) {
    if (!EVP_EncryptUpdate(ctx.get(), o, &len, u(body), int(body.size())) || !EVP_EncryptFinal_ex(ctx.get(), o + len, &fin) ||
        !EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, o + len + fin)) {
      fail(error, "AES-128-GCM");
      return std::nullopt;
    }
    out.resize(size_t(len + fin) + 16);
    return out;
  }
  std::string tag(in.substr(in.size() - 16));
  if (!EVP_DecryptUpdate(ctx.get(), o, &len, u(body), int(body.size())) ||
      !EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, tag.data()) ||
      EVP_DecryptFinal_ex(ctx.get(), o + len, &fin) <= 0) {
    fail(error, "the record doesn't decrypt");
    return std::nullopt;
  }
  out.resize(size_t(len + fin));
  return out;
}

std::string random_bytes(size_t n) {
  std::string out(n, '\0');
  RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), int(n));
  return out;
}

}  // namespace

std::string b64url(std::string_view bytes) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    uint32_t v = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8) | uint8_t(bytes[i + 2]);
    out += k[v >> 18], out += k[(v >> 12) & 63], out += k[(v >> 6) & 63], out += k[v & 63];
  }
  if (bytes.size() - i == 1) {
    uint32_t v = uint8_t(bytes[i]) << 16;
    out += k[v >> 18], out += k[(v >> 12) & 63];
  } else if (bytes.size() - i == 2) {
    uint32_t v = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8);
    out += k[v >> 18], out += k[(v >> 12) & 63], out += k[(v >> 6) & 63];
  }
  return out;
}

std::optional<std::string> b64url_decode(std::string_view text) {
  while (!text.empty() && text.back() == '=') text.remove_suffix(1);
  std::string out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : text) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '-' || c == '+') v = 62;
    else if (c == '_' || c == '/') v = 63;
    else return std::nullopt;
    acc = (acc << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += char((acc >> bits) & 0xff);
    }
  }
  if (bits >= 6) return std::nullopt;  // a lone character is no byte
  return out;
}

std::optional<Key> key_from_private(std::string_view private_raw, std::string* error) {
  if (private_raw.size() != 32) return fail(error, "a P-256 private key is 32 bytes"), std::nullopt;
  Group group(EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1));
  Bn d(BN_bin2bn(reinterpret_cast<const unsigned char*>(private_raw.data()), 32, nullptr));
  if (!group || !d) return fail(error, "P-256"), std::nullopt;
  Point pub(EC_POINT_new(group.get()));
  if (!pub || !EC_POINT_mul(group.get(), pub.get(), d.get(), nullptr, nullptr, nullptr)) {
    return fail(error, "not a P-256 private key"), std::nullopt;
  }
  unsigned char buf[65];
  if (EC_POINT_point2oct(group.get(), pub.get(), POINT_CONVERSION_UNCOMPRESSED, buf, sizeof buf, nullptr) != 65) {
    return fail(error, "P-256 point"), std::nullopt;
  }
  return Key{std::string(private_raw), std::string(reinterpret_cast<char*>(buf), 65)};
}

std::optional<Key> generate_key(std::string* error) {
  // A uniformly random scalar in [1, n-1] from OpenSSL's generator.
  Pkey key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"));
  BIGNUM* d = nullptr;
  if (!key || !EVP_PKEY_get_bn_param(key.get(), OSSL_PKEY_PARAM_PRIV_KEY, &d)) return fail(error, "keygen"), std::nullopt;
  Bn owned(d);
  std::string raw(32, '\0');
  if (BN_bn2binpad(d, reinterpret_cast<unsigned char*>(raw.data()), 32) != 32) return fail(error, "keygen"), std::nullopt;
  return key_from_private(raw, error);
}

std::optional<std::string> vapid_authorization(const Key& key, std::string_view audience, std::string_view subject,
                                               int64_t expires_s, std::string* error) {
  auto esc = [](std::string_view s) {
    std::string o;
    for (char c : s) {
      if (c == '"' || c == '\\') o += '\\';
      if (uint8_t(c) >= 0x20) o += c;
    }
    return o;
  };
  std::string header = b64url(R"({"typ":"JWT","alg":"ES256"})");
  std::string claims = b64url("{\"aud\":\"" + esc(audience) + "\",\"exp\":" + std::to_string(expires_s) + ",\"sub\":\"" +
                              esc(subject) + "\"}");
  std::string input = header + "." + claims;
  auto pkey = make_key(key.public_raw, key.private_raw, error);
  if (!pkey) return std::nullopt;
  MdCtx md(EVP_MD_CTX_new());
  size_t len = 0;
  if (!md || EVP_DigestSignInit(md.get(), nullptr, EVP_sha256(), nullptr, pkey.get()) <= 0 ||
      EVP_DigestSign(md.get(), nullptr, &len, reinterpret_cast<const unsigned char*>(input.data()), input.size()) <= 0) {
    return fail(error, "ES256"), std::nullopt;
  }
  std::string der(len, '\0');
  if (EVP_DigestSign(md.get(), reinterpret_cast<unsigned char*>(der.data()), &len,
                     reinterpret_cast<const unsigned char*>(input.data()), input.size()) <= 0) {
    return fail(error, "ES256"), std::nullopt;
  }
  // JWS wants r || s, 32 bytes each, not DER (RFC 7518 § 3.4).
  const unsigned char* p = reinterpret_cast<const unsigned char*>(der.data());
  Sig sig(d2i_ECDSA_SIG(nullptr, &p, long(len)));
  if (!sig) return fail(error, "ES256 signature"), std::nullopt;
  std::string rs(64, '\0');
  if (BN_bn2binpad(ECDSA_SIG_get0_r(sig.get()), reinterpret_cast<unsigned char*>(rs.data()), 32) != 32 ||
      BN_bn2binpad(ECDSA_SIG_get0_s(sig.get()), reinterpret_cast<unsigned char*>(rs.data()) + 32, 32) != 32) {
    return fail(error, "ES256 signature"), std::nullopt;
  }
  return "vapid t=" + input + "." + b64url(rs) + ", k=" + b64url(key.public_raw);
}

bool verify_jwt(std::string_view jwt, std::string_view public_raw) {
  auto dot = jwt.rfind('.');
  if (dot == std::string_view::npos) return false;
  auto rs = b64url_decode(jwt.substr(dot + 1));
  if (!rs || rs->size() != 64) return false;
  Sig sig(ECDSA_SIG_new());
  BIGNUM* r = BN_bin2bn(reinterpret_cast<const unsigned char*>(rs->data()), 32, nullptr);
  BIGNUM* s = BN_bin2bn(reinterpret_cast<const unsigned char*>(rs->data()) + 32, 32, nullptr);
  if (!sig || !r || !s || !ECDSA_SIG_set0(sig.get(), r, s)) {
    BN_free(r);
    BN_free(s);
    return false;
  }
  unsigned char* der = nullptr;
  int der_len = i2d_ECDSA_SIG(sig.get(), &der);
  if (der_len <= 0) return false;
  std::string err;
  auto pkey = make_key(public_raw, "", &err);
  MdCtx md(EVP_MD_CTX_new());
  bool ok = pkey && md && EVP_DigestVerifyInit(md.get(), nullptr, EVP_sha256(), nullptr, pkey.get()) > 0 &&
            EVP_DigestVerify(md.get(), der, size_t(der_len), reinterpret_cast<const unsigned char*>(jwt.data()), dot) == 1;
  OPENSSL_free(der);
  ERR_clear_error();
  return ok;
}

std::optional<std::string> encrypt(std::string_view plaintext, std::string_view ua_public, std::string_view auth_secret,
                                   std::string* error, const std::optional<std::string>& salt_in,
                                   const std::optional<Key>& sender_in) {
  if (plaintext.size() > kMaxPlaintext) {
    return fail(error, "a push message is at most " + std::to_string(kMaxPlaintext) + " bytes"), std::nullopt;
  }
  if (ua_public.size() != 65 || ua_public[0] != '\x04') return fail(error, "p256dh is not a P-256 public key"), std::nullopt;
  if (auth_secret.size() != 16) return fail(error, "auth is not 16 bytes"), std::nullopt;
  auto sender = sender_in ? sender_in : generate_key(error);
  if (!sender) return std::nullopt;
  std::string salt = salt_in ? *salt_in : random_bytes(16);
  auto secret = ecdh(*sender, ua_public, error);
  if (!secret) return std::nullopt;
  auto k = derive(*secret, auth_secret, ua_public, sender->public_raw, salt);
  // One record, the last: the plaintext and its delimiter, 0x02, with no padding.
  auto sealed = gcm(true, k.cek, k.nonce, std::string(plaintext) + '\x02', error);
  if (!sealed) return std::nullopt;
  std::string out = salt;
  out += char((kRecordSize >> 24) & 0xff), out += char((kRecordSize >> 16) & 0xff);
  out += char((kRecordSize >> 8) & 0xff), out += char(kRecordSize & 0xff);
  out += char(65);
  out += sender->public_raw;
  out += *sealed;
  return out;
}

std::optional<std::string> decrypt(std::string_view body, const Key& ua, std::string_view auth_secret, std::string* error) {
  if (body.size() < 16 + 4 + 1 + 65 + 17) return fail(error, "too short for aes128gcm"), std::nullopt;
  std::string_view salt = body.substr(0, 16);
  size_t idlen = uint8_t(body[20]);
  if (idlen != 65) return fail(error, "the key id isn't a P-256 public key"), std::nullopt;
  std::string_view as_public = body.substr(21, 65);
  auto secret = ecdh(ua, as_public, error);
  if (!secret) return std::nullopt;
  auto k = derive(*secret, auth_secret, ua.public_raw, as_public, salt);
  auto plain = gcm(false, k.cek, k.nonce, body.substr(86), error);
  if (!plain) return std::nullopt;
  // Strip padding back to the delimiter.
  while (!plain->empty() && plain->back() == '\0') plain->pop_back();
  if (plain->empty() || plain->back() != '\x02') return fail(error, "no last-record delimiter"), std::nullopt;
  plain->pop_back();
  return plain;
}

}  // namespace goober::webpush

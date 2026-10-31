#include "goober/base/hash.h"

// The SHA256_* calls are deprecated in OpenSSL 3 in favor of EVP, which allocates a context
// per hash; these keep the context in the object, which copies like any value.
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/sha.h>

#include "goober/base/encoding.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace goober::base {
// SHA-256 is OpenSSL's (it uses the CPU's SHA instructions where there are any): about four
// times what a portable loop does, on every content read, ETag, and log line.
static_assert(sizeof(SHA256_CTX) <= 128, "Sha256's storage holds a SHA256_CTX");

Sha256::Sha256() { reset(); }

void Sha256::reset() { SHA256_Init(reinterpret_cast<SHA256_CTX*>(ctx_)); }

void Sha256::update(std::string_view data) {
  if (data.empty()) return;  // an empty view may have no data pointer at all
  SHA256_Update(reinterpret_cast<SHA256_CTX*>(ctx_), data.data(), data.size());
}

std::array<uint8_t, 32> Sha256::finish() {
  std::array<uint8_t, 32> out{};
  SHA256_Final(out.data(), reinterpret_cast<SHA256_CTX*>(ctx_));
  reset();
  return out;
}

std::array<uint8_t, 32> sha256(std::string_view data) {
  Sha256 h;
  h.update(data);
  return h.finish();
}

std::string sha256_hex(std::string_view data) {
  auto digest = sha256(data);
  return hex_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

std::string sha1_hex(std::string_view data) {
  unsigned char digest[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
  return hex_encode(std::string_view(reinterpret_cast<const char*>(digest), sizeof digest));
}

std::optional<std::string> sha256_file_hex(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  Sha256 h;
  std::string buf(1 << 16, '\0');
  while (in) {
    in.read(buf.data(), std::streamsize(buf.size()));
    h.update(std::string_view(buf.data(), size_t(in.gcount())));
  }
  if (in.bad()) return std::nullopt;
  auto digest = h.finish();
  return hex_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

HmacSha256::HmacSha256(std::string_view key) {
  // RFC 2104: a key longer than a block is hashed first; then inner and outer pads.
  uint8_t block[64] = {};
  if (key.size() > 64) {
    auto k = sha256(key);
    std::memcpy(block, k.data(), k.size());
  } else {
    std::memcpy(block, key.data(), key.size());
  }
  uint8_t ipad[64];
  for (int i = 0; i < 64; ++i) {
    ipad[i] = block[i] ^ 0x36;
    opad_[i] = block[i] ^ 0x5c;
  }
  inner_.update(std::string_view(reinterpret_cast<char*>(ipad), 64));
}

std::array<uint8_t, 32> HmacSha256::finish() {
  auto inner_digest = inner_.finish();
  Sha256 outer;
  outer.update(std::string_view(reinterpret_cast<char*>(opad_), 64));
  outer.update(std::string_view(reinterpret_cast<const char*>(inner_digest.data()), inner_digest.size()));
  return outer.finish();
}

std::array<uint8_t, 32> hmac_sha256(std::string_view key, std::string_view data) {
  HmacSha256 h(key);
  h.update(data);
  return h.finish();
}

std::string hmac_sha256_hex(std::string_view key, std::string_view data) {
  auto mac = hmac_sha256(key, data);
  return hex_encode(std::string_view(reinterpret_cast<const char*>(mac.data()), mac.size()));
}

bool equal_constant_time(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

uint32_t crc32(std::string_view data) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char ch : data) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

}  // namespace goober::base

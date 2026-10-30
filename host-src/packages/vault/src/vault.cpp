#include "goober/vault/vault.h"

#include "goober/base/chacha20.h"
#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/base/random.h"

namespace goober::vault {

namespace {

constexpr size_t kSalt = 16, kTag = 32;

std::string_view bytes(const Key& k) { return std::string_view(reinterpret_cast<const char*>(k.data()), k.size()); }

std::string tag_of(const Key& mac, std::string_view salt_and_ct, std::string_view context) {
  // Lengths first, so no two (body, context) pairs share bytes.
  std::string msg = std::to_string(salt_and_ct.size()) + ":" + std::string(salt_and_ct) + std::to_string(context.size()) + ":" +
                    std::string(context);
  auto t = base::hmac_sha256(bytes(mac), msg);
  return std::string(reinterpret_cast<const char*>(t.data()), t.size());
}

void derive(const Key& key, std::string_view salt, Key* enc, Key* mac) {
  *enc = base::hmac_sha256(bytes(key), "goober-vault-enc\n" + std::string(salt));
  *mac = base::hmac_sha256(bytes(key), "goober-vault-mac\n" + std::string(salt));
}

bool same(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char d = 0;
  for (size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
  return d == 0;
}

}  // namespace

Key tenant_key(const Key& vault, std::string_view tenant) {
  return base::hmac_sha256(bytes(vault), "goober-seal\n" + std::string(tenant));
}

std::string context(std::string_view tenant, std::string_view service, std::string_view owner) {
  return std::to_string(tenant.size()) + ":" + std::string(tenant) + std::to_string(service.size()) + ":" +
         std::string(service) + std::to_string(owner.size()) + ":" + std::string(owner);
}

std::string seal(const Key& key, std::string_view plaintext, std::string_view ctx) {
  std::array<uint8_t, kSalt> salt{};
  base::os_random_fill(salt);
  std::string s(reinterpret_cast<const char*>(salt.data()), salt.size());
  Key enc{}, mac{};
  derive(key, s, &enc, &mac);
  std::string ct(plaintext);
  std::array<uint8_t, 12> nonce{};
  base::ChaCha20 c(enc, nonce);
  c.apply(reinterpret_cast<uint8_t*>(ct.data()), ct.size());
  std::string body = s + ct;
  return std::string(kPrefix) + base::base64url_encode(body + tag_of(mac, body, ctx));
}

std::optional<std::string> unseal(const Key& key, std::string_view sealed, std::string_view ctx) {
  if (!sealed.starts_with(kPrefix) || sealed.size() > 65536) return std::nullopt;
  auto raw = base::base64url_decode(sealed.substr(kPrefix.size()));
  if (!raw || raw->size() < kSalt + kTag) return std::nullopt;
  // One spelling per value: padding or stray bits that decode the same are refused, so a sealed
  // value can be compared and stored as text.
  if (base::base64url_encode(*raw) != sealed.substr(kPrefix.size())) return std::nullopt;
  std::string_view all(*raw);
  std::string_view body = all.substr(0, all.size() - kTag), tag = all.substr(all.size() - kTag);
  Key enc{}, mac{};
  derive(key, body.substr(0, kSalt), &enc, &mac);
  if (!same(tag, tag_of(mac, body, ctx))) return std::nullopt;
  std::string pt(body.substr(kSalt));
  std::array<uint8_t, 12> nonce{};
  base::ChaCha20 c(enc, nonce);
  c.apply(reinterpret_cast<uint8_t*>(pt.data()), pt.size());
  return pt;
}

std::string seal_for(const Key& tenant_key, std::string_view tenant, std::string_view service, std::string_view owner,
                     std::string_view plaintext) {
  std::string body = seal(tenant_key, plaintext, context(tenant, service, owner));
  return std::string(kPrefix) + std::string(owner) + "." + body.substr(kPrefix.size());
}

std::optional<Opened> open_for(const Key& tenant_key, std::string_view tenant, std::string_view service,
                               std::string_view sealed) {
  if (!sealed.starts_with(kPrefix)) return std::nullopt;
  std::string_view rest = sealed.substr(kPrefix.size());
  auto dot = rest.find('.');
  if (dot == std::string_view::npos || dot > 64) return std::nullopt;
  std::string owner(rest.substr(0, dot));
  for (char c : owner) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == ':')) return std::nullopt;
  }
  auto pt = unseal(tenant_key, std::string(kPrefix) + std::string(rest.substr(dot + 1)), context(tenant, service, owner));
  if (!pt) return std::nullopt;
  return Opened{owner, std::move(*pt)};
}

}  // namespace goober::vault

#pragma once

// Sealed values (design/machine/034.SpeakingHttp.md § 4): "gsv1." and base64url of a 16-byte
// salt, the ChaCha20 ciphertext, and an HMAC-SHA256 tag over the salt, the ciphertext, and a
// context (tenant, service, owner) that isn't stored. The host seals; a box only keeps them.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace goober::vault {

using Key = std::array<uint8_t, 32>;
inline constexpr std::string_view kPrefix = "gsv1.";

// The tenant's sealing key: HMAC-SHA256 of the vault key over "goober-seal" and the tenant.
Key tenant_key(const Key& vault, std::string_view tenant);

// The context a value is bound to, unambiguous for any strings.
std::string context(std::string_view tenant, std::string_view service, std::string_view owner);

std::string seal(const Key& key, std::string_view plaintext, std::string_view context);
// The plaintext, or nullopt for anything that isn't a value sealed under key and context.
std::optional<std::string> unseal(const Key& key, std::string_view sealed, std::string_view context);

// A sealed value as a box holds it: "gsv1." the owner, ".", and the sealed body. The owner (a
// user id, not a secret) travels in the clear so the host can rebuild the context, and is
// authenticated with the rest: changing it fails the check.
std::string seal_for(const Key& tenant_key, std::string_view tenant, std::string_view service, std::string_view owner,
                     std::string_view plaintext);
struct Opened {
  std::string owner, plaintext;
};
std::optional<Opened> open_for(const Key& tenant_key, std::string_view tenant, std::string_view service,
                               std::string_view sealed);

}  // namespace goober::vault

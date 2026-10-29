#pragma once

// Web Push (design/machine/782.PWA.md § 5): VAPID (RFC 8292) and message encryption
// (RFC 8291, aes128gcm per RFC 8188), on P-256 with OpenSSL's libcrypto. Pure functions:
// sending is the host's, through the outbox.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace goober::webpush {

// base64url without padding, both ways (RFC 4648 § 5).
std::string b64url(std::string_view bytes);
std::optional<std::string> b64url_decode(std::string_view text);

// A P-256 key: the private scalar (32 bytes) and the uncompressed public point (65 bytes).
struct Key {
  std::string private_raw;
  std::string public_raw;
};
std::optional<Key> generate_key(std::string* error);
// The key for a private scalar, its public point computed.
std::optional<Key> key_from_private(std::string_view private_raw, std::string* error);

// The VAPID Authorization header for a push service: "vapid t=<JWT>, k=<public key>", the
// JWT signed ES256 over {aud, exp, sub}. aud is the endpoint's origin, like
// https://fcm.googleapis.com; sub a mailto: or https: contact; exp unix seconds, at most a
// day ahead.
std::optional<std::string> vapid_authorization(const Key& key, std::string_view audience, std::string_view subject,
                                               int64_t expires_s, std::string* error);

// Encrypts one push message for a subscription's keys (p256dh: 65 bytes, auth: 16 bytes) as
// one aes128gcm record: salt, rs 4096, the sender's ephemeral public key, the ciphertext.
// Plaintext is at most kMaxPlaintext bytes. salt and sender are for tests; normally random.
inline constexpr size_t kMaxPlaintext = 3072;
std::optional<std::string> encrypt(std::string_view plaintext, std::string_view ua_public, std::string_view auth_secret,
                                   std::string* error, const std::optional<std::string>& salt = std::nullopt,
                                   const std::optional<Key>& sender = std::nullopt);
// The reverse, as a browser does it (for tests and checks).
std::optional<std::string> decrypt(std::string_view body, const Key& ua, std::string_view auth_secret, std::string* error);

// Verifies an ES256 JWT's signature with a public point (for tests).
bool verify_jwt(std::string_view jwt, std::string_view public_raw);

}  // namespace goober::webpush

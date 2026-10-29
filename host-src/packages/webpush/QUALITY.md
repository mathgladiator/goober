# webpush

Web Push's cryptography (`design/machine/782.PWA.md` § 5): VAPID (RFC 8292) as an ES256 JWT,
and aes128gcm message encryption (RFC 8291, RFC 8188), on P-256 with a pinned, static
OpenSSL libcrypto.

## Quality bar

- **Checked against the RFC.** RFC 8291's Appendix A example encrypts byte for byte, and
  decrypts back; a flipped bit fails to decrypt.
- **No hand-rolled curve math.** Every elliptic-curve and AES operation is OpenSSL's; this
  package only arranges the bytes the RFCs name. HKDF is RFC 5869 with base's HMAC-SHA-256.
- **Random from OpenSSL.** Keys, salts, and the per-message sender key come from OpenSSL's
  generator; tests may pass a salt and sender to reproduce the RFC.
- **Bounded.** A message is at most 3072 bytes, so one record always fits the 4096 every push
  service takes.

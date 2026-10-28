# backup

The parts of S3 backups that need no host (design/machine/019.Backups.md): sealing files,
page-level deltas of SQLite databases, and the names of what goes in the bucket.

## Quality bar

- **Nothing unauthenticated is ever written.** `unseal_file` checks the HMAC over the whole
  file before decrypting a byte, and the output appears (by rename) only when it is whole.
- **Keys are per file.** The encryption and MAC keys come from the backup key and a random
  salt, so no two sealed files share a ChaCha20 key stream, and nonce reuse can't happen.
- **A rebuilt database is the snapshot, byte for byte**, or nothing: `apply_delta` checks the
  SHA-256 the delta recorded, so a delta applied to the wrong night fails loudly.
- **Stamps sort as times** and parse back exactly; a restore point exists only for a night
  that is still in the bucket.
- The primitives under it (SHA-256, HMAC, ChaCha20) are tested against RFC vectors in `base`.

# base

Small, dependency-free building blocks: hashing (SHA-256, CRC-32), encodings (hex, base64,
percent), the host name grammar, durable file writes, seeded randomness, and time.

## Quality bar

- Every function is pure or touches only the file it is given.
- Hashes and encodings are tested against published vectors, not against themselves.
- Durable writes always go temp file, fsync, rename, fsync the directory. Nothing in the
  host writes a file it owns any other way.
- The name grammar is the single definition of `[a-z][a-z0-9_]*`, at most 63 characters,
  and not starting with `goober_`. Nothing else in the host re-implements it.

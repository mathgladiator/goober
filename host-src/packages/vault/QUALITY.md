# vault

Sealed values (`design/machine/034.SpeakingHttp.md` § 4): secrets a tenant's box keeps in its
own database without being able to read them.

## Quality bar

- **Encrypt-then-MAC**, as backups seal files: ChaCha20 under a key derived per value from a
  random salt, and HMAC-SHA256 over the salt, the ciphertext, and the context.
- **Bound to its context.** The tenant, service, and owner are authenticated but not stored; a
  value used anywhere else fails its check, in constant time.
- **Per-tenant keys** from the vault key, so one tenant's key opens nothing of another's.

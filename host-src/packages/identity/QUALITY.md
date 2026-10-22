# identity

The built-in identity service of `013.Identity.md`, v1 subset: passwordless sign-in by an
emailed one-time code, opaque session tokens, grant roles, and an audit table. One SQLite
database per realm (`$tenant.db`, and `_host.db` for host identities).

## Quality bar

- Every secret is stored as a `sha256:` hash: codes, session tokens. A stolen database file
  yields nothing usable.
- Codes expire in 10 minutes, are single use, and lock after 5 wrong tries. Starts are
  throttled per address. Unknown and known addresses look the same to the caller.
- A user_id is never reused (`AUTOINCREMENT`). A session belongs to exactly one realm.
- Every change of identity or role lands in the audit table, in the same transaction.
- Tests drive the service directly with a capturing mailer and a fixed clock.

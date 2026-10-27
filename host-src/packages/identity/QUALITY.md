# identity

The built-in identity service of `013.Identity.md`, v1 subset: passwordless sign-in by an
emailed one-time code, opaque session tokens, grant roles, an audit table, and OAuth 2.1
(`src/oauth.cpp`, design/machine/009.OAuth.md): clients, PKCE codes, rotating refresh
tokens, and the device flow. One SQLite
database per realm (`$tenant.db`, and `_host.db` for host identities).

## Quality bar

- Every secret is stored as a `sha256:` hash: codes, session tokens. A stolen database file
  yields nothing usable.
- Codes expire in 10 minutes, are single use, and lock after 5 wrong tries. Starts are
  throttled per address. Unknown and known addresses look the same to the caller.
- A user_id is never reused (`AUTOINCREMENT`). A session belongs to exactly one realm.
- Every change of identity or role lands in the audit table, in the same transaction.
- OAuth: S256 only; codes last 60 seconds and work once, and a replayed code revokes what it
  bought. Refresh tokens rotate, and presenting a rotated one revokes the whole grant.
  Redirects match exactly, except that a loopback port may differ. Person clients never use
  the device flow; dynamically registered clients are always agents.
- Tests drive the service directly with a capturing mailer and a fixed clock.

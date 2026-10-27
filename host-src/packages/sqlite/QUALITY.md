# sqlite

A small RAII wrapper over SQLite, shared by the identity realms, the attachment ledger,
and tenant databases: one connection, prepared statements, typed binds and columns.

## Quality bar

- Every connection opens in WAL mode with `synchronous=FULL`, foreign keys on, and a
  five-second busy timeout, so a crash never loses a committed row.
- Errors throw `std::runtime_error` carrying SQLite's message; nothing returns a bare code.
- Statements are finalized by their destructor, so an early return can't leak one.

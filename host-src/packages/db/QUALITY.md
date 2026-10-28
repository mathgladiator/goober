# db

Tenant databases for boxes (`design/machine/008.Databases.md`): JSON schemas stored once,
SQLite files per tenant grown only by expanding schemas, direct SQL checked by SQLite's
authorizer against a strategy's grants, one transaction per database per request, and
operator queries.

## Quality bar

- **A statement can't see past its grant.** Every table, column, and operation is checked
  by SQLite's authorizer at prepare time, so the host and SQLite can't disagree about what
  a statement touches. PRAGMA, ATTACH, transactions, DDL, and impure functions are refused.
- **Schemas only expand.** A changed column or index is refused, before anything is
  written, and an upload is checked against every tenant's existing copy first.
- **What the host declared is recorded in the database** (`goober_schemas`,
  `goober_columns`, `goober_indexes`), so comparisons never depend on parsing SQL.
- **Every statement is bounded** by a row limit and a step budget.
- **A request's writes to one database land together or not at all**: a session rolls back
  unless it is committed, including when it is destroyed. Across databases, each commits on
  its own (no two-phase commit). The outbox's database commits last, so an element is never
  enqueued for writes that didn't land.
- **A strategy that may write takes the write lock with its first statement**, so a read
  followed by a write never fails with SQLITE_BUSY_SNAPSHOT under concurrent writers.

# Unifying Tables and Event Sources

> Status: machine-authored and **not ratified**. Food for thought for
> `design/human/420.Migration.md` and the data flow design. Nothing here is a decision.

The migration design keeps the host simple by refusing every schema change it can't emulate, and by pushing real restructuring into boxes that write a new table. That raises a deeper question, which the human posed directly:

> Is it a more mature way to deal with schemas if data is really just a large number of writes that are stored differently?

This document explores that idea: persist every committed write as an event, treat tables as one way of storing those events, and let migrations become "store the same events a different way."

## 1. What goober already has

Goober is closer to an event source than it looks:

- **Every write is named and typed.** Since the data flow design replaced "write": true with named writes of five kinds, every change to a table is already a discrete, declared command: user_by_id.rename, orders_by_customer.place_order. Each one has a fixed shape in a pinned schema version.
- **Writes commit as bundles.** A request's writes land together, with a request_id, at least once, idempotently, as described in the outbox and data shape designs.
- **The bus already publishes on commit.** Topics are bumped when a strategy's writes commit, as described in the bus design.
- **Traces already join everything by request_id.**

So the host already sees, for every commit: who (the reserved identifiers), what (the named write and its values), where (the database and table), and why (the route and request_id). A table only keeps the result.

## 2. The proposal: a write log per database

Persist every committed write as an entry in an append-only log, beside the tables, in the same database file:

```sql
CREATE TABLE goober_write_log (
	seq INTEGER PRIMARY KEY,              -- per-database, monotonic
	at INTEGER NOT NULL,                  -- commit time, unix ms
	request_id TEXT NOT NULL,
	box TEXT NOT NULL,
	version TEXT NOT NULL,                -- the box version that wrote it
	schema_version TEXT NOT NULL,         -- like 'core13'
	write TEXT NOT NULL,                  -- like 'user_by_id.rename'
	kind TEXT NOT NULL,                   -- insert, update, upsert, patch, patch-except, delete
	row_key BLOB NOT NULL,                -- the primary key it touched
	payload BLOB NOT NULL                 -- the values, in the writer's schema version
);
```

The tables stay exactly what they are: the fast, queryable current state. The log is a second record of the same commits, written in the same transaction, so the two can never disagree inside a database.

## 3. What a log buys

| capability | how |
| --- | --- |
| Data migrations without touching the old box | A migration box subscribes to a table's log from a sequence number and builds a new table from it, catching up and then following. This is the "tapping into existing data flow" future section of the migration design. |
| Backfills that are just replays | Building a new table from scratch is reading the log from seq 0, or from a snapshot. |
| Indexes for data services | Search, analytics, and the agent's planner in the Trad Wife System can each consume the log into their own shape, without new access patterns on the primary tables. |
| Audit and "what happened?" | Every change is recorded with who, which box version, and which request, which the operating panel and support consoles can read. |
| Undo and repair | A bad box version's writes can be found by version and request_id, and compensated precisely. |
| Shadow comparison | A shadow version's would-be writes can be compared against the primary's log entry for entry. |

## 4. Automatically converting a database into an event source

A database that already exists has no history, but it can still become an event source without anyone noticing:

1. **Snapshot.** At a chosen moment, the host records one synthetic insert per existing row, in the current schema version, marked as a snapshot at seq 0 to n.
2. **Turn on the log.** From then on, every commit appends its writes in the same transaction.
3. **Consumers start from the snapshot.** A consumer that wants the whole state reads the snapshot and then follows. One that only wants changes starts from the current seq.
4. **Compact.** Old entries are folded into a new snapshot on a schedule, so the log doesn't grow without bound, while consumers that are behind keep reading from their position until they catch up.

During a migration, this turns step 2 and 3 of the migration design's data migration (write both, backfill) into one move: the migration box consumes the old table's log from its snapshot and writes the new table, and the old box never changes.

## 5. Is this the more mature model?

**Arguments for:**

- **It matches how goober already thinks.** Writes are named commands with fixed shapes. Persisting them is a small step from declaring them.
- **Migrations become storage changes.** If the log is the truth, a table is a view, and changing a table's shape is rebuilding a view from the same events. No data is lost by a contraction, since the events still hold the dropped column. Renames, splits, and type changes that the host refuses today could become "replay into a new table with an adapter box," without the developer writing dual-write code.
- **It fits the blue-collar OS goal.** An old box keeps writing the same events forever; the platform can store them however it likes.
- **It fits the use cases.** Personal public infrastructure wants feeds of what changed; the Trad Wife System wants an agent that reads what happened in a household and plans from it. Both are consumers of a log.

**Arguments against:**

- **Events have schemas too.** A log written in core13 must be readable forever, so every event shape is a contract that can never change, which is the same versioning problem moved one level down. Event-sourced systems solve it with upcasters, which accumulate and slow every read.
- **Storage and write cost.** Every write is stored twice, and compaction is a new operational job.
- **Privacy and deletion.** A log remembers what a table forgot. Deleting a person's data means rewriting or encrypting history, as in crypto-shredding with per-user keys, which ties into the keys design.
- **Ordering is only per database.** Each tenant database has its own seq, so cross-database consumers see many streams, not one, which is fine for indexing but not for anything that needs a global order.
- **The table is the right truth for most products.** A plumber's job list doesn't need its history replayed. A log for every table may be machinery most products never use.

## 6. A middle path

Treat the log as an **opt-in property of a table**, not the foundation of every database:

- A .schema can mark a table as logged. Logged tables record every committed write; others don't.
- A data migration can **temporarily** log the old table, from a snapshot, for as long as the migration box needs to follow it, and then stop.
- Data services and agents can subscribe to logged tables through an internal route, at least once and in order per database.
- Deletion of a person's data deletes their entries in the log too, by row_key and the reserved identifiers that wrote them.

This keeps the migration design's simplicity (tables are the truth, changes are immediate, hard changes go through new tables), while giving the hard cases a much better tool than hand-written dual writes.

## 7. Open questions for the human

1. **Is the log the truth, or a by-product?** If it is the truth, tables become rebuildable views, and contractions stop losing data. If it is a by-product, it is a convenience for migrations and indexing.
2. **Should logging be per table, temporary for migrations, or always on?**
3. **How long do events live?** Forever, until compaction, or for a fixed window?
4. **Do event shapes follow the same rules as schemas,** immutable per version and projected for consumers, or do consumers always read in the writer's version?
5. **How does a person's data get deleted from history?** Rewrite, tombstones, or per-user encryption keys.
6. **Is this the external dependency that the bus should become?** The bus already publishes on commit; a log is a bus that remembers.

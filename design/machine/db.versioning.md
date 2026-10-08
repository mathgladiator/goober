# Live Schema Versioning: How Other Systems Do It

> Status: machine-authored and **not ratified**. This is a survey written to inform
> `design/human/420.Migration.md`. Descriptions of other systems come from prior knowledge
> and were **not** re-checked against their current documentation; verify specifics before
> relying on them.

Every long-lived system eventually has to change the shape of its data while it is still serving traffic. The interesting differences between databases aren't whether they can do it, but **who carries the burden**: the database, an external tool, or the developer's application code. This survey looks at the main families of approach, what each costs the developers who use it, and what goober can take from each.

## 1. The families

| family | examples | where versioning lives |
| --- | --- | --- |
| In-place DDL with online algorithms | PostgreSQL, MySQL (InnoDB), SQLite | In the database engine, one ALTER at a time |
| Shadow-table copy tools | gh-ost, pt-online-schema-change, Vitess / PlanetScale | In an external tool that builds a new table beside the old one |
| Distributed online schema change | Google Spanner, CockroachDB (F1 lineage) | In the database's own multi-step state machine |
| Versioned views over one table | pgroll, Reshape | In a layer of per-version views and triggers |
| Migration frameworks | Rails, Django, Alembic, Flyway, Liquibase, Prisma | In application code, as an ordered list of scripts |
| Schemaless with application versioning | MongoDB, DynamoDB, document stores | Entirely in application code, per document |
| Schema registries and evolution rules | Protobuf, Avro, Confluent Schema Registry, FoundationDB Record Layer | In compatibility rules checked before a schema is accepted |
| Event sourcing with upcasters | Event stores, Axon-style frameworks | In code that translates old events into new shapes on read |

## 2. In-place DDL

**PostgreSQL** runs DDL inside transactions, so a failed migration rolls back cleanly. Many changes are cheap: adding a nullable column, or one with a non-volatile default, only touches the catalog in modern versions. Others take a lock that blocks reads and writes for the whole table while they work, like changing a column's type, and building an index without CONCURRENTLY blocks writes. Developers learn which statements are safe the hard way, usually after an outage.

**MySQL** offers online DDL algorithms, some of them instant, but many changes still copy the table, and every DDL needs a brief metadata lock that can queue behind a long transaction and stall everything behind it. This is why large MySQL shops reach for the shadow-table tools below.

**SQLite** supports only a few changes in place: renaming a table or column, adding a column, and, in recent versions, dropping one. Anything else is a rebuild: create the new table, copy the rows, drop the old one, and rename, all in one transaction. Its pragma user_version gives a single integer to track which migration a file has had. For small databases, like one per tenant, a rebuild is cheap and simple; for a large shared database, it is a long exclusive lock.

**Consequences for developers:**
- Every migration is a separate decision about locks and duration, which depends on table size, database version, and statement.
- The application and the schema must be deployed in a careful order, since the database only has one shape at a time.
- Renames and type changes are effectively forbidden on hot tables, so teams accumulate unfortunate names forever.

## 3. Shadow-table copy tools

**gh-ost** and **pt-online-schema-change** build a new table in the new shape beside the old one, copy rows across in the background, keep it in step with ongoing writes (gh-ost by reading the binary log, pt-osc with triggers), and then swap the two in a short cutover. **Vitess** and **PlanetScale** turn this into a workflow: schema changes are submitted as deploy requests, reviewed, run without blocking, and, notably, **revertible**, since the old table is kept in step for a while after cutover, so a bad change can be undone without losing writes made since.

**Consequences for developers:**
- Large tables can change without downtime, at the cost of time and double the disk while the copy runs.
- The cutover is still a single moment where application and schema must agree, so the application must tolerate both shapes around it.
- Reviewing schema changes as deploy requests, separate from code, becomes a normal part of shipping.

## 4. Distributed online schema change

**Spanner** and **CockroachDB** follow the approach described in Google's F1 work: a schema change moves through intermediate states, where a new column or index is first only maintained on deletes, then on writes, and only then becomes visible to reads, with backfills running in between. Every node may briefly be on adjacent schema versions, and the protocol guarantees that no two nodes are ever more than one state apart, so data stays consistent throughout.

**Consequences for developers:**
- Schema changes are asynchronous jobs that can take hours on large tables, and developers have to wait for them before relying on the result.
- Some combinations, like changing schema and data in the same transaction, are restricted.
- The database carries nearly the whole burden of consistency, which is the most developer-friendly position in this survey, but the database also decides the pace.

## 5. Versioned views over one table

**pgroll** and **Reshape** are the closest relatives of goober's design. Each migration is expressed declaratively, and the tool creates a separate schema of views for each version, so old application instances keep querying the old version's views while new instances query the new version's views, against the same underlying table. Writes through either version are kept consistent with triggers, new columns are backfilled in batches, and when no old instances remain, the migration is completed and the old views and columns are dropped. A migration that goes wrong can be rolled back before completion.

**Consequences for developers:**
- Old and new application versions can run side by side during a deploy, which removes the ordering problem entirely.
- The developer chooses which version to connect to, usually by setting a search path per deployment.
- Triggers on every write add cost while a migration is open, and complex transformations still need hand-written SQL.

## 6. Migration frameworks

**Rails, Django, Alembic, Flyway, Liquibase, and Prisma** keep migrations as an ordered list of scripts in source control, applied in sequence and recorded in a table. They solve the bookkeeping, meaning which scripts have run, but leave every operational question to the developer. Experienced teams adopt the **expand and contract** convention on top: add the new shape, deploy code that writes both, backfill, deploy code that reads the new one, and only then remove the old one, which turns one logical change into several deploys.

**Consequences for developers:**
- Migrations are easy to write and easy to get wrong, since the framework doesn't know about locks, table sizes, or traffic.
- Expand and contract is a discipline teams have to enforce by convention and code review.
- Rolling back is often a myth: a down migration that drops a column can't restore the data it dropped.

## 7. Schemaless, with versioning in the application

**MongoDB** and other document stores accept any shape, so versioning moves into the application. The common pattern is a schemaVersion field in each document, with code that understands every version that might still exist, and either upgrades documents lazily when they are next written or runs a background job to upgrade them all. **DynamoDB** has the same property; secondary indexes can be added while the table is live, but any change to the data itself is an application-level backfill.

**Consequences for developers:**
- Deploys never wait on the database, which feels fast.
- The code must handle every version that might still be stored, forever or until a backfill finishes, and that knowledge lives scattered through the application.
- Bugs show up as documents in shapes nobody expected, long after the change that created them.

## 8. Schema registries and evolution rules

**Protobuf** and **Avro** don't migrate stored data at all; they make every change compatible by rule. Fields have stable numbers or names, new fields are optional with defaults, and removed fields are reserved forever so they can't be reused. **Confluent Schema Registry** enforces compatibility modes (backward, forward, or full) before a new schema is accepted. The **FoundationDB Record Layer** applies similar evolution rules to stored records and builds new indexes online, tracking each index's state until it is ready to serve reads.

**Consequences for developers:**
- Many changes are simply forbidden, and the tooling says so before anything ships.
- Old and new readers coexist indefinitely without any migration at all, for the changes that are allowed.
- Anything outside the rules, like a real restructuring, still needs a separate migration.

## 9. Event sourcing with upcasters

Systems built on event sourcing never change stored events. Instead, when an event's shape changes, an **upcaster** translates old events into the new shape every time they are read, and projections are rebuilt from the full history when the read model changes.

**Consequences for developers:**
- The history is never lost, and new read models can be built from it at any time.
- Upcasters accumulate, and every read pays to run the chain.
- Rebuilding a projection over years of history can take a very long time.

## 10. The burden, side by side

| approach | downtime risk | who handles old and new code together | rollback | ongoing cost |
| --- | --- | --- | --- | --- |
| In-place DDL | High for some statements | The developer, by careful ordering | Hard | None after |
| Shadow-table tools | Low | The developer, around cutover | Possible with Vitess-style revert | Double disk during copy |
| Distributed online change | Very low | The database | Limited | Slow backfills |
| Versioned views | Very low | The tool, through per-version views | Before completion | Triggers on every write while open |
| Migration frameworks | Depends on the developer | The developer, by convention | Often a myth | None after |
| Schemaless | None at the database | The developer, in every reader, forever | Not applicable | Every version handled in code |
| Evolution rules | None | Nobody, for allowed changes | Not needed | Restricted vocabulary |
| Upcasters | None | The framework, on read | Not needed | Every read, forever |

## 11. What goober can take from this

The migration design already sits in the best-supported position: like **pgroll and Reshape**, each box sees its own schema version while the host keeps old and new shapes consistent, and like **Spanner and CockroachDB**, the platform rather than the developer carries the consistency burden. A few lessons are worth carrying over explicitly:

- **Make the expand step a state machine.** The F1 states (maintained on delete, then on write, then visible to reads) are a proven way to add a column or index with no window of inconsistency. The host's expand and backfill steps could adopt the same states per database.
- **Keep migrations revertible until contraction.** Vitess keeps the old table in step after cutover so a bad change can be undone. Since goober already writes both shapes until contraction, it gets this almost for free, and the API should expose it as a revert.
- **Borrow the registry's compatibility modes.** Many schema changes can be classified ahead of time as compatible or not, the way Avro and Protobuf rules do. The checker can apply the same rules to .schema versions, so that safe changes, like adding an optional column, need only a map, and unsafe ones demand an adapter box and a louder warning.
- **Reserve removed names.** Protobuf never reuses a removed field number. A removed column's name should stay reserved in later schema versions, so an old box's write to it can never be confused with a new column of the same name.
- **Decide how long pipes live.** Schemaless systems and upcasters both show the cost of supporting every old version forever. Goober's goal of products that never change means some pipes may live for years; chaining them (13 to 14 to 15) is cheaper to write but costs every read and write a hop per version, while keeping direct pipes from every live version costs more to write but stays fast. This is the most consequential open question for the migration design.
- **Expect double writes to matter.** Every approach that keeps two shapes in step pays for it in write cost and disk while the migration is open. Prompt contraction, and quotas that account for migrations in progress, keep that cost bounded.
- **Small databases change everything.** Most of the pain in this survey comes from large shared tables. With a database per tenant, a SQLite rebuild of one tenant's table is fast, so goober can afford simple, safe mechanics per database that would be unthinkable on a single large table, and it can migrate tenants gradually, one file at a time.

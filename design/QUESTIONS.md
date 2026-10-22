# Open Questions for a First Version

> Status: machine-authored and **not ratified**. Compiled 2026-10-21 from every document in
> `design/human/`, `design/planning/planning.to.plan.md`, and the reviews through
> `design/machine/feedback-2026-10-08.md`. Each question limits or blocks building v1.
> Answers belong in the human docs. When one is answered, delete it here.

## How to read this

Every question has a gate:

- **L1**: decide before line one. Code can't start without it.
- **F:x**: decide before feature x ships.
- **CUT**: deferred if v1 drops a scope. The question names the cut.

Citations name the file and the section heading.

## A proposed v1 scope

The CUT tags assume this scope. It is a proposal, not a decision.

| area | in v1 | out of v1 |
| --- | --- | --- |
| Runtime | One machine. Sync, solo workers. C and Rust generators. | Async, shared, service mode, cluster. |
| Routes | http routes and host-level timers. | internal, sse, mail, mcp, event, upgrade, data-driven timers. |
| Data | sqlite3 by_id and by_index with named writes. Pure expansion only. | query, sql, append and by_cursor (sources), contractions, upgrade boxes. |
| Identity | Email-code and password sign-in, browser sessions, grant roles, grantable and revocable. | Passkeys, device flow, dynamic registration, host diving into tenants. |
| Content | Static content and configs. Every route conflict is refused. | Markdown, templates, bundles, working set, traffic plans. |
| Deploy | deployment.mode go. Outbox for send-email@1 with a dev provider. | slow, shadow, splits, comparison reports. |
| Ops | Version upload, get, list, and host health. | Quotas, replay corpus, rollups, heat. |

## A. Foundation and runtime

1. **WASM engine.** [L1] Which engine does the C++ host embed: wasmtime (C API), WAMR, Wasmer, or V8? How are fuel and timeouts enforced: fuel metering or epoch interruption? No doc names an engine. timeout-ms, trap accounting, heat, and isolate spin-up cost all depend on it. *Cites:* `000.Introduction.md` § The Foundational Thinking; `010.Manifest.md` § Start with Hello World; `300.OperatingPanel.md` § Box instrumentation.

Let's go with wasmtime. I want both fuel metering along with epoch interruption via timeouts. I want each WASM box to be isolated to a thread that can be managed an monitored.

2. **Build system, package layout, third-party baseline.** [L1] The planning doc wants one directory per package (router, server_http), a $package/QUALITY.md each, and stable third-party packages as module seams. It names no build tool (CMake, Bazel, Buck2), HTTP library, JSON parser, SQLite build flags, or test framework. *Cites:* `design/planning/planning.to.plan.md`; `design/machine/PHILOSOPHY.md` (a candidate only).
 
I want a reasonable build tool that AI can manage. Pick the best one with C++ that provides incremental builds and good unit test running capability.
 
3. **Host concurrency model.** [L1] Thread per request, or an event loop plus a worker pool? Where does a sync isolate block on a database call? Which thread owns each SQLite writer and does group commit? Write-set batching and one writer per file both assume an answer. *Cites:* `010.Manifest.md` § Start with Hello World; `025.DataShape.md` § Write sets and bundles.

I ultimately want a thread pool that checks out WASM black boxes, so multiple WASM boxes can be active and blocked waiting for an event. This means that the ultimate model is asynchronous, so the event loop and worker pool. The question then is whether or not wasm-time supports suspending the box until an event comes back from whereever. At core, I want a reasonable balance between what network can execute along with how the CPU is managed by linux such that I can also pin to a core and maximize black box performance.

4. **Allowed module imports.** [L1] May a box import anything besides goober host functions, like wasi_snapshot_preview1 for libc or Rust std? Which Rust target: wasm32-unknown-unknown or wasm32-wasip1? This decides what the checker refuses at upload, and whether generated code can use malloc. *Cites:* `000.Introduction.md` § The Foundational Thinking (no POSIX inheritance); `110.Languages.md`; `060.Files.md` § Toward a real file system.

Absolutely not. I want absolute control over what the box has access too.

5. **The goober-abi@1 import table.** [L1] What are the import module name, the function names, and their signatures? 120 gives layout rules, goober_payload_size, and goober_payload_pickup, but not the field-by-field fallback it says to build first, the respond functions, or error returns. *Cites:* `120.TheBoundary.md` § The fallback: copy field by field, § The dance.

Use good practices like goober_ and then generate under gen_$schema_$method etc.

6. **Clock, randomness, and logging.** [L1] No host function gives a box the time, random bytes, or a debug log. Rows need created_at, and agents need a debug channel. One option is goober_now_ms and goober_random, with time pinned per request so replay is deterministic. Upgrade boxes in 420 must get neither. *Cites:* `045.Time.md` § The rules of time; `330.Stability.md` § Proving the host didn't change anything; `420.Migration.md` § Forcing an upgrade with a box.

These are going to be part of the goober ABI, and I want them controllable by the host such that the host controls time, randomness, and can capture logging as a side effect in the outbox. Ideally, I want to be able to predict the entire sequence of events leaving the box with only non-determinism being latency of answering requests.

7. **One dispatch contract.** [L1] http routes dispatch through goober_on_request(int32_t signal), but timers export one function per name, like daily_digest(const struct projects_row*). Which one is the reactor contract? Event and upgrade routes need the same answer. *Cites:* `010.Manifest.md` § Start with Hello World; `045.Time.md` § What a fire is.

Instead of the event loop, it may make sense to maximize the reactor contract and go with best practices for wasm such that maybe each route has its own handler.

8. **Isolate limits.** [L1] No manifest field sets max linear memory or stack. What does behavior.instances.default mean under solo? What happens when goober_on_start traps? *Cites:* `120.TheBoundary.md` § When the dance doesn't fit; `150.DeepManifest.md` § behavior.

Let's have reasonable limits defined, any parameters to config the wasm should have sane defaults the manfiest can override.

9. **Where the checker and generators live.** [L1] Are they in the host binary, a separate CLI, or both, and in what language? Agents build off-host, yet the host checks the upload, so both places must run the same code. Who assigns signal and writes it back into the manifest? *Cites:* `010.Manifest.md` § Start with Hello World; `310.Deployments.md` § The life of a version.

They are a shared library with seperate tools, MCP pathways, and then are part of the validation path. The generators are exposed as either binaries for claude or MCP tools for groks.

10. **Generated-name collisions.** [L1] Route names, strategy labels, external labels, and counters share one C namespace (user_by_id_save_profile, mailer_send_message, goober_counter_*). What is the collision rule? How does a type name become a struct name (JobWithCustomer to struct job_with_customer)? *Cites:* `020.DataFlow.md` § Generated code; `036.Json.md`; `041.ReservedRoutes.md` § Reserved names.

I want all names globally unique within a host, and bad names are rejected. No conflicts are allowed in the system.

11. **Host configuration file.** [L1 for the parts v1 uses] The pieces are scattered: identity (013), domains.verification (015), operations (300), and quotas (390). Nothing covers the data directory, listen address, platform domain, host timezone, vault, or mailer. *Cites:* `feedback-2026-10-08.md` §12.

I want a single JSON configuration file to define everything along with the capability to reload it from disk via a signal.

12. **On-disk layout.** [L1] Is a database one file, or storage/sqlite3/$database/$version/$table? Where is the tenant's directory that holds the upload ledger, idempotency keys, and outbox? Where do non-tenant databases like jobs live? Where do source databases live? *Cites:* `000.Introduction.md`; `020.DataFlow.md` § Patterns, tenants, and limits; `025.DataShape.md` § Second; `060.Files.md` § The upload ledger.

I want storage/sqlite3/{tenant}/internal.db for all interal per tenant then storage/sqlite3/{tenant}/databases/$database/$version.db and tables are a part of the database schema/version.

13. **Tenant id grammar.** [L1] !tenant, like bobs_plumbing, is pasted into database names, folders, and key names. What characters and length are allowed, and is it immutable? Is a realm keyed by tenant or by primary domain? *Cites:* `015.Domains.md` § The domain registry; `013.Identity.md` § Identity is per domain.

I want [a-z][a-z0-9_]* so just a-z characters then numbers and underscore are added to the mix after the initial letter.

## B. Manifest and checker

14. **The .schema format.** [L1] This was a P0 on 2026-10-08 and is still open. It needs:
    - the syntax;
    - how column types map to the 036 vocabulary;
    - defaults, nullability, indexes, and primary keys;
    - where the version lives (the file name, like core13.schema, or inside the file);
    - how partial schemas compose;
    - how a table is marked as a source (023).

    Data codegen, timer row structs, projection, and upgrade boxes all read it. *Cites:* `020.DataFlow.md`; `023.Sourcing.md` § A source database holds only sources; `420.Migration.md` § A schema is a partial set of tables.

Invent one that is sane that provides everything SQL needs, but is also easy to parse and maintain. Perhaps, a good JSON format for a schema.

15. **Route-to-strategy binding.** [L1] Strategies are box-wide. No field says which strategies a route may reach. How does {customer_id} in index-is bind to a route parameter, and what happens on a route that has no such parameter? The checker's per-route reach, bundle reports, and signatures all need this. *Cites:* `020.DataFlow.md` § Where scope comes from; `025.DataShape.md` § Write sets and bundles.

The routes will define new parameters which then feed into the data sources as variables. A route that has no such parameter will get a runtime error when trying to talk to a data source that doesn't have a route. This will produce a yellow level warning for runtime potential errors. This error will also manifest a counter of incomplete data access which the AI can monitor for to ensure is clean.

16. **Caller-chosen ids inside a tenant.** [L1] 020 allows {name} scope only for read-only public strategies. Then how does GET /jobs/{job_id}, behind member requirements, read job 42 by_id? Many examples write through index-is: {customer_id}. There are two options:
    - (a) The per-tenant database is the guardrail, so {name} is fine inside tenant_{!tenant}. The docs lean this way.
    - (b) Keep the rule and lose by-id access.

    *Cites:* `020.DataFlow.md` § Where scope comes from, § Writes by pattern; `150.DeepManifest.md` § Data.

The per-tenant database selection is the primary guardrail, so we want to keep it simple but the key requirement is auditing for weirdness.

17. **The role sweep.** [L1 for examples and tests] Under 013, owner/{!tenant}, member/{!tenant}, and friends mean "a role scoped to a thing named bobs_plumbing", so every such route is unreachable as written. *Cites:* `013.Identity.md` § Grant roles; `feedback-2026-10-08.md` §7.1.

It isn't unreachable if they have all the tokens. This is where the path is gated by a disjunction of and statements.

18. **Artifact packaging and the version hash.** [L1] What is the container (tar, zip, multipart)? How are the manifest and file order canonicalized? Which hash? How do relative references like "schema": "core13.schema" resolve? Is the hash taken before or after deploy-time defaults and pins are written in? *Cites:* `310.Deployments.md` § The artifact; `150.DeepManifest.md` § Rules every field obeys; `330.Stability.md`.

I want everything to have a mirror to disk such that the host operator can inspect it, and I also want a WAL for those files as well such that I can be crash proof. Ultimately, I'd like this to work with an out of the box backup solution that can bundle up everything to S3. Also, I would like the ability to leverage the sqlite3 WAL for recovery as well. So, any file we control, should have a unified host WAL for file flushes. Each sqlite3 database will need to be able to to setup for litestream as well.

19. **Fields used in prose but missing from 150.** [F: per feature] 150 says it wins conflicts, so each of these needs a ratified field or a removal:
    - the outbox result route;
    - route-level redirects, CORS origins, and route-level headers;
    - timer timeout-ms;
    - the binary response;
    - the event and upgrade protocols, and the append and by_cursor patterns, from 023 and 420.

    *Cites:* `035.TheOutBox.md`; `037.Http.md`; `045.Time.md`; `023.Sourcing.md`; `420.Migration.md`.

For now, I want to get to the point where I can evolve the host organically as I want to play.

20. **Pins at v1.** [F: deploy] Which pins exist at v1 (goober-abi@1, c@1, rust@1, and so on)? The host carries every pinned version forever, so the v1 set should be small. *Cites:* `330.Stability.md` § Everything else outside the box is pinned.

this is going to be at the goober ABI.

21. **The checker's cost model.** [CUT: a structural checker only] Worst-case fitting inside timeout-ms, health thresholds, and guardrails need units nobody has defined. *Cites:* `025.DataShape.md` § Can a box even behave well?; `feedback-2026-10-06.md` §6.1.

I want baseline units, and each manifest has operational control over each box.

## C. Data

22. **Write-set semantics.** [L1] Are writes buffered and overlaid on reads, or applied in a held-open transaction or savepoint? What isolation exists between concurrent requests on one file? How does the by_index group check survive group commit? When an insert collides at commit, after the write function returned true, what do the box and the caller see? *Cites:* `025.DataShape.md` § Write sets and bundles; `020.DataFlow.md` § Writes by pattern.

I want writes buffered and the commited together before a response is sent out. The ideal is to minimize torn writes using the outbox such that I can bundle the entire outbox together, write that to disk in a WAL, then execute the bundle together, then write a delete to the WAL file indicating the bundle executed. This means recovery can execute everything after playing the log to build up the bundles in memory.

23. **Multi-database bundles.** [L1] "Holds the response until the bundle's first commit is durable": which commit is first when a bundle touches a tenant database, another database, the realm database, and the outbox? What happens when a later part fails for good? 023 adds another case: an append to a source database beside a table write. *Cites:* `025.DataShape.md` § Write sets and bundles; `023.Sourcing.md` § Writing to a source.

This is why we need a system level WAL such that we can bundle everything together between databases and have a measure of transaction clarity between databases.

24. **Database creation and DDL ownership.** [L1] When is tenant_{!tenant} created? Who applies DDL: approving the box, or schema_apply? Two boxes naming database jobs share it. Is that intended? *Cites:* `020.DataFlow.md`; `410.API.md` § Data, schemas, and migrations.

They will be created on demand by the host. The host is responsible for what methods is provided to the box and those methods can get routed directly to a database as well to the new database. The host, and by proxy, the operator is responsible for how data flows during mirations due to approvals. And yes, two boxes can share a database as each SQlite3 datbase is guarded by a lock.

25. **Are schemas part of a box version or separate artifacts?** [L1] 310 puts them in the artifact. 410 has schema_create, schema_apply, and schema_delete. An upgrade box (420) pairs a box with a schema version, so the answer shapes it. *Cites:* `310.Deployments.md` § The artifact; `410.API.md`.

Schemas are an isolated API and storage concept, so they have their own logs. They can only be upgraded in predictable ways like adding columns with new defaults. Each manifest binds to a schema, and each schema belongs to a database concept in terms of the version. Each manifest binds to a specific version in a database's history.

26. **Primary keys.** [L1] Must every table have one INTEGER primary key minted by the host? What about composite keys? Which int64 fields cross JSON as strings: every int64, or only ids? How does a realm database get id ranges? *Cites:* `021.PrimaryKeys.md`; `036.Json.md`.

There is one database responsible for vending ID ranges to specific boxes, and so the schemas concept of auto-incrementing ID or a UUID is replaced with a system minted id. The schema can allow composite keys as well, but that requires a different API to create. For now, we don't worry about it, but it is a door left open.


27. **The by_index read API.** [L1 for by_index] Only by_id reads are shown. What do page and cursor functions look like, and in what order do rows come? *Cites:* `020.DataFlow.md` § Generated code; `028.Query.md`.

Ultimately, I want it to be an iterator design such that the host can control paging and buffering.

28. **The platform database schema.** [L1] domains, id_counter, and id_ranges are given. The version registry, approval records, audit, access logs, and counters are not. Event cursors (023) need a home too. *Cites:* `305.Approvals.md`; `300.OperatingPanel.md`; `023.Sourcing.md` § The cursor.

Whatever is good. You decide.
29. **Durability and backup.** [F: data anyone cares about] Is Litestream a sidecar? How are new per-tenant files registered with it? Does the id counter share their failure domain? A source database is append-only and never compacted. Does it get its own backup path? *Cites:* `099.TheSystem.md`; `021.PrimaryKeys.md` § Range leases; `023.Sourcing.md`.

For now, litestream is a side car the host will eventually manage. For now, I want a WAL for the entire system as well as a directory footprint that existing backup tools can manage.

30. **File handles.** [F: many tenants] What is the open and close policy, and what are the WAL settings, across thousands of tenant files plus realm, keys, and source databases? *Cites:* `013.Identity.md`; `064.Keys.md`.

I don't really know since I don't have data. At this time, I'm thinking of tens of tenants. Chances are, if I get this working on one host well, I may just stick with that and scale up the hosts horizontally to provide a different kind of system..

## D. Sources and migration (023, 420)

These came out of today's drafting of 023 and 420.

31. **What "an event can only grow" means.** [L1 for sources] 023 reads it two ways: the log only gets longer, and an event's shape only gains columns. Is that the intent? *Cites:* `023.Sourcing.md` § An event can only grow.

The event's shape can only gain columns. This is about having an immutable long term log of things that have happened. This isn't well thought out at the moment, but we will eventually have a way to collapse the event source into something meaningful. Until then, let's have a basic thing with cursors.

32. **Sources and deletion.** [F: sources] A source never forgets, but a person can ask to be forgotten. Options:
    - rewrite history;
    - tombstones;
    - per-person envelope keys that are destroyed on request (crypto-shredding), which ties into 064.

    Until this is answered, sources can't hold personal data. *Cites:* `023.Sourcing.md`; `064.Keys.md`; `design/machine/db.unify.event.source.md` §7.

Event sources will not hold personal data.

33. **Retention and quotas for sources.** [F: sources] Forever by default. Do 390's row and byte quotas count events, and what happens at a red line, when an append can't be refused without losing what happened? *Cites:* `390.Quotas.md`; `023.Sourcing.md`.

For now, forever is fine since this is a new feature. At a future point, it may make sense to have a tailer of the log to also truncate the log on consumpion. This implies that a box that truncates the log can only run after all other tailers have successfully accepted the event, and it also creates a warning that adding a tailer will have incomplete data stream.

34. **Many event types in one stream.** [L1 for sources] 023 makes each source one event type, sharing one sequence per source database, so a handler sees several types in commit order. The alternative is one table holding a union of types. Which one? *Cites:* `023.Sourcing.md` § A source database holds only sources.

Each data source is a single event type.

35. **A poison event.** [F: event routes] 023 stalls a handler at a failing event until a new box version fixes it. May an operator skip an event, and is that an approval? *Cites:* `023.Sourcing.md` § The cursor.

Operators should be able to delete an event, and events can also be marked as poisin if they fail and skipped such that operators can view bad events. We will monitor, but we should have a reasonable config policy per event stream.

36. **Cursors and shadows.** [F: event routes with shadow mode] Does a shadow or secondary version get its own cursor, a read-only copy of the primary's, or none? *Cites:* `023.Sourcing.md` § The cursor; `310.Deployments.md` § Shadowing.

It gets its own cursor, and it can even read data the real one is producing. At core, shadowing is soft as a way of validating a new version of a box doesn't crash and does stuff within reason. The key is that the shadow can not commit data.

37. **Exactly-once cursors.** [F: event routes] 023 stores the cursor in the handler's own database when the bundle touches only one database. Is a host-owned table inside a tenant database acceptable? 045's "where timer state lives" asks the same thing. *Cites:* `023.Sourcing.md` § The cursor; `045.Time.md` § Where timer state lives.

Yes, a host owned table per tenant database is acceptable. This can also be directly in the tenant database like a goober_ special table.

38. **Can a box read a source directly?** [F: sources] 023 says no: there are no indexes, and handlers build every lookup. A product showing a job's history must then keep a table of it. *Cites:* `023.Sourcing.md` § What a handler can't do.

The box can read only what it is allowed to do. Eventually, we will provide more ways to read more advanced queries using sql parsing and then eventually making a version of C with languaged integrated query.

39. **Old readers during a forced upgrade.** [F: upgrade boxes] 420 requires a reads handler (new row to old row) while old readers remain, or else it waits for them to retire. Is a downgrade ever lossy enough to refuse? *Cites:* `420.Migration.md` § Forcing an upgrade with a box.

There is preserving the WASM generated as it may be a one-shot from some Grok session, and then there is a responsible engineering team with the code in a repo. As such, I want us to be able to long term support a WASM generated as long as possible. This means that the host has the challenge of making the manifest's definition as good as possible, but there can be lossy reads such that default values are required when deleting a column for legacy data.

40. **Large databases and forced upgrades.** [F: upgrade boxes] 420 rewrites each database in one transaction while its writes wait. That is fine for a small tenant file. What is the limit, and what happens to a large shared database? *Cites:* `420.Migration.md` § Forcing an upgrade with a box.

Not sure, this is an experiment.

41. **A fleet half-upgraded.** [F: upgrade boxes] If some databases fail their rows handler, they stay at from, and boxes pinned to to can't serve them. Are those tenants down for new code until someone fixes it? Should an upgrade be all-or-nothing across the fleet? *Cites:* `420.Migration.md` § Forcing an upgrade with a box.

For now, I don't want to worry about a fleet. This is a consideration, but the hosts will be able to proxy everything. Ideally, we follow best practices for distributed systems with high availabilities.

42. **Naming.** [L1 for these features] Five new platform names appear in 023 and 420. Each needs a ratified spelling, and they belong in 150, 010's protocol list, and 020's write-kind table:
    - the event and upgrade protocols;
    - the append and by_cursor patterns;
    - the append write kind;
    - the fields from, events, rows, writes, and reads;
    - write keys spelled box.strategy.write.
    *Cites:* `023.Sourcing.md`; `420.Migration.md`.

Have fun naming things, keep it professional but consistent.

43. **Migration in v1.** [CUT: pure expansion only] Recommendation: v1 accepts new tables and new columns with defaults, and refuses everything else. Contractions, emulated type changes, and upgrade boxes come later. *Cites:* `420.Migration.md`; `025.DataShape.md` § First.

Go with recommendation. 

## E. Identity and domains

44. **Browser sessions.** [L1 for identity] How does a browser request carry identity to a box route: a cookie mapped to an opaque token? What are the cookie format, the CSRF mechanism, and the access, refresh, and code lifetimes in numbers? *Cites:* `013.Identity.md` § Sessions, schema, and the login API; `070.UI.md` § Keeping browsers safe.

Cookie with opaque token. Pick from the best security practices, and I want everything user facing as a host level configuration with tenant overrides.


45. **The dev mailer.** [L1 for email-code sign-in] keys/system/mailer and send-email@1 are named, but the contract isn't written. A laptop binary needs a provider that logs to the console or a file. *Cites:* `038.Mail.md` § Sending mail; `330.Stability.md` § Host capabilities; `005.Goals.md` § Developer Velocity and the Sandbox.

For now, the dev mailer will be a log with a pointer to the MIME email. Amazon SES is also the primary way to send email for now, but emails should be pluggable.

46. **The platform domain and realm.** [L1] Both are used, and neither has a config field. *Cites:* `013.Identity.md` § Bootstrapping the host; `300.OperatingPanel.md` § The reserved surface.

This should be part of the host config as well.

47. **Whose template styles sign-in.** [F: identity] A domain has many manifests. Which one's layout styles sign-in, and what do "fixed slots" mean? *Cites:* `070.UI.md` § Styling the sign-in.

Ultimately, I envision a single page templating system such that the host then makes a request to pull a style configuration for the domain being asked. In some sense, identity is too important to give the tenant any meaningful control over, so we give them a config that then configures the identity UI with banner images, colors, etc. For the system, this is just a single step recursion to pull a static file.

48. **Tenant-from-path on host-role routes.** [CUT: no support console in v1] 020 § Patterns forbids it outside internal routes. 013's support-console allows it. *Cites:* `feedback-2026-10-08.md` §7.5.

Pick the options that maximize security.

49. **Host roles.** [F: operators] 013 uses host:support. 300 and 410 use host:owner, operator, and observer. How does a platform session reach a tenant domain when cookies are exact-domain? *Cites:* `013.Identity.md` § Diving into a tenant; `410.API.md` § Roles.

There will need to be a process such that a tenant domain can be forced to vend a token for the operator, so this is a handshake. Use best security practices to do the cookie stamping.

## F. Protocols

50. **HTTP router precedence.** [L1] Within one box, does /jobs/new beat /jobs/{slug : string}, or is it a 019 conflict? What about trailing slashes, percent-decoding, and case? *Cites:* `019.Conflicts.md` § What counts as a conflict.

The slashes determine the rule, and conflicts within the rule are static identifiers like new beat out an arbitrary slug. We trim trailing slashes at the root and disallow empty slugs and then collapse double slashes to a single slash until only (/{thing})*

51. **Host error bodies.** [L1] What body comes back with a host 4xx or 5xx, a validation failure, or a quota refusal? *Cites:* `037.Http.md` § The status vocabulary.

When the system produces a 4xx or 5xx, the system will turn that around to return a /error-message/400.html such that tenants and the host can define it. If those don't exist, then a generated error page is produced with an "Uh Oh!"

52. **OpenAPI subset gaps.** [L1] Nullable as an OpenAPI 3.1 type array? Which string formats? Is a bare "json" with no type legal? 036 says no; 020, 040, and 300 use it. *Cites:* `036.Json.md` § A strict subset, § The invariants.

Go with best practices.

53. **External calls.** [F: external; CUT except send-email@1] How are credentials attached? What is the syntax of require paths? How does a sync box receive a response struct (030 returns int32_t, 035 returns bool)? What is the SSRF policy for base? *Cites:* `030.TheLargerEnvironment.md`; `035.TheOutBox.md`.

Go with best practices that maximize security.

54. **Where the outbox lives** for a box with no tenant database, or with several. [F: outbox] *Cites:* `035.TheOutBox.md` § Starting small.

The outbox will be a bundle written to a WAL before executing.

55. **Timers.** [F: timers] What is the cron dialect? Where does tzdata come from, and where is the host timezone set? Where is "last fired per row" stored? *Cites:* `045.Time.md`.

Go with best practices. Each tenant can have its own database, and that's a good place to put a row per timer.

Pull tzdata during setup and have a cached copy such that host operators can update it and pull from OS if need be.

56. **Deferred protocols.** [CUT] Each of these has open wire-level questions that v1 can skip:
    - sse and the bus: where topic sequences persist;
    - mail receive: the Stalwart hook format and reply-token signing;
    - internal: blocking sync calls and idempotency retention;
    - mcp: the protocol revision and transport;
    - uploads: the transfer protocol and preprocessing libraries.

    *Cites:* `040.TheBus.md`; `038.Mail.md`; `039.InternalMessaging.md`; `050.AgentsMCP.md`; `060.Files.md`.

We will incrementally work on this.

## G. Operations and deploy

57. **How v1 approves anything.** [L1] 410 says an approval not covered by policy happens only in the panel's UI. 005 says to put a box and a manifest in, and it answers. Does v1 need a UI, a dev-mode auto-approve, or a local CLI? Nothing deploys without an answer. *Cites:* `410.API.md`; `005.Goals.md`; `305.Approvals.md` § Policies.

I want a UI for operators, so that will be a thing to vibe up.

58. **Diff kinds.** [F: approvals] There is no schema kind, and no kind for an upgrade box. What makes a contraction never-waivable? *Cites:* `305.Approvals.md` § What an approval reads; `feedback-2026-10-08.md` §7.4.

We will explore as I play.

59. **The vault.** [F: keys] Which vault and which AEAD? "No vault, no start" conflicts with a laptop binary unless a dev vault exists. *Cites:* `064.Keys.md` § Envelope encryption.

The vault will be a sqlite3 database, and I want each key to be an encrypted JSON bundle (with 64 bytes of random noise prefixed). The key for the encryption will be in the host config, but this will eventually be pulled from the vault.

60. **The minimum ops surface.** [F: ops] Proposed: version_upload, version_get, version_list, host_health, and an access log on disk. *Cites:* `300.OperatingPanel.md`; `410.API.md`.

I want whatever will be required since we will work on this together.

61. **Cluster placement.** [CUT the cluster; keep the registry seam] 400 describes two placement systems: lease on demand (§ Routing), and consistent hashing for timers (§ Timers in a cluster). The fleet store is undecided. *Cites:* `400.Cluster.Service.md`; `feedback-2026-10-08.md` §7.3.

For now, v1 is all about a single host.

## H. Contradictions v1 would trip on

For all contradictions, I want whatever is simplest to get testing since I have found a great way to work with AI about this.

1. Writable {name} scope (020 § Writes by pattern, 150 `index-is`) vs read-only-public {name} (020 § Where scope comes from). See question 16.
2. owner/{!tenant} and friends vs realm-wide roles (013). See question 17.
3. One file per database, changed in place (025, 120, 420) vs storage/sqlite3/$database/$version/$table (000, 020).
4. Expand-then-contract later (025 § First) vs immediate contraction (420).
5. Timer tenancy: tenant only through tenant-field (018) vs implicit {!tenant} partitioning (045, 400). 023 copies the implicit partitioning for event routes, so the answer covers both.
6. "A timer never templates" (045) vs target.path filled from a column.
7. "No anonymous JSON" (036) vs "response": "json" (020, 040, 300).
8. Shadow means no network (305) vs shadow-calls "allow" (310).
9. An immutable hash (150) vs defaults and pins written in at deploy (330).
10. Schema inside the artifact (310) vs separate schema APIs (410).
11. Signal dispatch (010) vs named timer exports (045).
12. int32_t (030) vs bool (035) for the same mailer_send_message.
13. host:support (013) vs host:owner, operator, observer (300, 410).
14. Approval only in the UI (410) vs it answers out of the box (005).
15. Email in the same binary (005) vs renting Stalwart (099).
16. Upload files named by upload id (060 § The upload ledger) vs by row id (060 § Serving a file) vs by original name (070).

## The shortest path to building

Decide these first, in this order. With the v1 scope above, everything else can wait.

1. WASM engine (1).
2. Build system and baseline libraries (2).
3. Concurrency model (3).
4. Allowed imports (4).
5. The ABI import table, with clock, randomness, and logging (5, 6).
6. The dispatch contract (7).
7. Artifact format and hash (18).
8. Write-set semantics (22, 23).
9. The .schema format (14).
10. Route-to-strategy binding and caller-chosen ids (15, 16).
11. The role sweep (17).
12. Browser sessions (44).
13. The dev mailer (45).
14. Host config and on-disk layout (11, 12).
15. How v1 approves (57).

At core, the short path here is getting WASM engine working with HTTP and just enough MCP to deploy. Then we add databases, and that's a lot to get crack'n.
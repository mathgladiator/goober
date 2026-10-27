# CLAUDE.md

## Phase: design, with a first host to play with

Goober is still mostly in the **design and context-building phase**. A first host (v1)
lives in `host-src/`, built at the human's request so they can play with the design. Its
decisions are recorded in `design/machine/005.FirstVersion.md`. On 2026-10-26 the human
asked Claude to keep executing the whole design, toward software they run for themselves
and can build products on. `PROGRESS.md` at the repo root is the plan and the honest
account: every human doc's status, what is faked or missing, and a checklist Claude works
through. Keep it true with every change, and grow the code along that plan, not beyond it.

## The host (`host-src/`)

- C++20, CMake with Ninja presets, GoogleTest through CTest; dependencies under `~/.local`
  (Boost, GoogleTest, wasmtime v49.0.4 in `~/.local/opt/`).
- `cmake --preset dev && cmake --build --preset dev && ctest --preset dev` builds and tests
  everything (`just test` does the same). `host-src/README.md` covers running and playing.
- The `justfile` at the repo root drives play: `just run`, `just setup`, `just mcp`,
  `just examples`, and `just db` / `just sql`. The default host directory is `demo/`, which
  is ignored by git. When the human wants it, Claude keeps the demo host running itself
  (`just run` in the background) and restarts it after host changes.
- One package per directory under `host-src/packages/`, each with its own library, tests,
  and `QUALITY.md`. The underscore in a package name is a namespace (`server_http` is
  `goober::server::http`). Keep changes siloed per package so each is reviewable.
- Every new behavior gets a test in its package. Run the full suite before committing.

## Design documents

Read these before doing design work:

- `design/human/`: written and maintained by the human. **Authoritative and
  protected.** Treat it as the source of intent. Your edits here are limited to
  grammar, syntax, and clarity, so the text makes sense while saying exactly what the
  human meant. Never add, remove, or change ideas, positions, or decisions. If a
  clarity fix would shift the meaning, or if you think the content itself is wrong,
  propose it in conversation or in `design/machine/` instead.
  - `design/human/000.Introduction.md`: the introduction and foundational thinking (the bet, boxes, manifests, storage, process model, the five arenas).
  - `design/human/005.Goals.md`: what the experiment is for and against, and how we will know it worked.
  - `design/human/010.Manifest.md`: manifest shape, protocols, the reactor ABI, behavior, sharing rules, naming rule, schema table.
  - `design/human/012.Configs.md`: configs as box-less manifests; per-property priority; rewrites with `{name}`/`{name*}` patterns.
  - `design/human/013.Identity.md`: per-domain identity realms, grant roles (`/` namespaces), deny-by-default, grantable/revocable, login API.
  - `design/human/015.Domains.md`: domain registry (primary + redirects), `domain/*` context, optional verification, per-domain content.
  - `design/human/018.Reserved.md`: `{!name}` reserved identifiers set only by the host; `{name}` is caller-chosen; no context field.
  - `design/human/019.Conflicts.md`: route conflicts keyed by domain; priority and share; shadowing reports.
  - `design/human/020.DataFlow.md`: context sources, `data.$provider.$strategy`, access patterns, per-tenant databases, SQL forcing solo.
  - `design/human/021.PrimaryKeys.md`: host-minted 64-bit ids, counter -> host range -> thread slice, no reuse.
  - `design/human/023.Sourcing.md`: sources: append-only, expand-only event tables in source databases; `event` routes with per-route cursors.
  - `design/human/025.DataShape.md`: storage shapes, sequencers, atomic write sets (optional per route), torn-write reporting.
  - `design/human/028.Query.md`: three levels of reach (patterns, manifest iterators, direct SQL), budgets.
  - `design/human/030.TheLargerEnvironment.md`: networking through the host, OpenAPI services, `require` restrictions, `keys/...` credentials.
  - `design/human/035.TheOutBox.md`: effectful calls as outbox elements, delivered after commit with idempotency keys.
  - `design/human/036.Json.md`: OpenAPI-only type vocabulary (shorthands, `token`), strict subset, safe vs. breaking changes.
  - `design/human/037.Http.md`: declared query/headers/bodies; status vocabulary; validation, CORS, ETags in the host.
  - `design/human/038.Mail.md`: mail routes, verified senders, reply-token capabilities, special providers, mailers.
  - `design/human/039.InternalMessaging.md`: `internal` routes with `callers`, context propagation, no cycles, non-atomic sync calls.
  - `design/human/040.TheBus.md`: pub/sub as monotonic signals, coalesced SSE subscriptions, publish-on-commit.
  - `design/human/041.ReservedRoutes.md`: the single list of host-owned prefixes, /internal/ hooks, reserved addresses and names.
  - `design/human/045.Time.md`: timer routes and data-driven schedules (plain `-field` refs).
  - `design/human/050.AgentsMCP.md`: MCP tools/resources mirroring HTTP; agents limited to mcp routes; isolate agents in their own box.
  - `design/human/060.Files.md`: upload verb, preprocessing, ledger states, asset routes, GC.
  - `design/human/064.Keys.md`: vault-held root key, envelope encryption, `keys/<scope>/...`.
  - `design/human/070.UI.md`: content negotiation, embedded static content and templates, objects, backing tree.
  - `design/human/099.TheSystem.md`: the operational surroundings; rent the edge to keep the host small.
  - `design/human/110.Languages.md`: C and Rust generators at launch; per-language plan and consequences.
  - `design/human/120.TheBoundary.md`: wasm32 C ABI, fallback copies, pickup + relocation fast path, untrusted pointers.
  - `design/human/150.DeepManifest.md`: THE field reference for manifests; box manifests vs. configs; reachability.
  - `design/human/300.OperatingPanel.md`: operations surface, access logs, instrumentation, queues, counters, heat, traces.
  - `design/human/305.Approvals.md`: version registry, manifest diffs, policies, go/slow/shadow modes.
  - `design/human/310.Deployments.md`: version lifecycle, shadowing, comparison reports, splitting with guardrails.
  - `design/human/315.StaticBundles.md`: bundles as content configs, unbundling, rogue-content checks, splitting.
  - `design/human/320.WorkingSet.md`: editable draft overlay for editors/agents, submitted as a new version.
  - `design/human/325.ManagingKeys.md`: key metadata in the panel, missing/rejected keys are 500s, urgent key requests.
  - `design/human/330.Stability.md`: capabilities (send-email@1) with swappable providers, adapter-box redirects, pins, dependency-only updates, replay.
  - `design/human/390.Quotas.md`: host-config quotas per domain (users, rows, bytes, uploads), red lines, enforcement points.
  - `design/human/400.Cluster.Service.md`: service mode, ownership registry, fenced leases, routing, recovery.
  - `design/human/410.API.md`: the full operator API catalog (HTTP + MCP), approval UI-only.
  - `design/human/420.Migration.md`: partial schemas, change kinds applied immediately, refused changes, upgrade boxes that force a change, data migrations via boxes.
  - `design/human/999.Mobile.md`: the dumb mobile terminal: a person OAuth client (PKCE, loopback any port), `/terminal/v1/` routes on a box, alarms as rules, idempotent decisions, linted markdown, push as a nudge; eight open decisions.
- `design/QUESTIONS.md`: questions still open after the first round, plus new ones from building v1.
- `design/machine/`: drafted by Claude. Proposals until a human ratifies them.
  - `design/machine/005.FirstVersion.md`: the v1 decisions from the human's answers: wasmtime,
    CMake, the two thread pools, the v1 ABI, storage and the host WAL, HTTP rules, MCP tools.
  - `design/machine/006.LocalIdentity.md`: v1 identity (emailed codes, cookie sessions, realms
    per tenant, route requirements), local mode, operator tokens, setup, and the justfile.
  - `design/machine/007.Content.md`: content as its own vector: pages (markdown, fragments,
    documents), Mustache templates with layouts, listings and feeds, and attachments at
    `/attachment/$id.$ext` with uploads and MCP tools.
  - `design/machine/008.Databases.md`: tenant databases: JSON schemas that only expand,
    `data.sqlite3` strategies with `pattern: "sql"`, `goober_sql`, request transactions,
    and the operator's db tools and CLI.
  - `design/machine/009.OAuth.md`: OAuth 2.1 for the terminal and agents: person and agent
    clients, PKCE codes with consent, rotating refresh tokens, the device flow, bearer tokens,
    and `"agents": true`.
  - `design/machine/010.Terminal.md`: the dumb terminal's backend as two example boxes,
    `terminal` (the person) and `desk` (agents), with decisions in their own table so no agent
    can decide; checked by `just terminal-e2e`.
  - `design/machine/011.Types.md`: a box's OpenAPI types document in 036's strict subset, and
    routes that declare body, query, request-headers, and response, checked by the host.
  - `design/machine/012.Agents.md`: `/mcp` on every domain: box tools for bearer tokens,
    filtered discovery, typed inputs, an audit log, and RFC 9728 metadata.
  - `design/machine/013.SecurityReview.md`: the first internal security review: every
    finding, its fix and test, what isn't fixed and why, and what was checked and sound.
  - `design/machine/014.Time.md`: timers: every-ms and cron routes, `by_timer` sources firing
    per row in each row's zone or once at an at-field, catch-up, and the desk's 6am briefing.
  - `design/machine/PHILOSOPHY.md`: engineering philosophy carried over from an earlier
    project. It references C++, CMake, Boost, and `goo::system::System`, none of which
    has been decided for goober. Treat it as a candidate, not a rule.
  - `design/machine/feedback-2026-10-04.md`: a long-form review of every human doc, with
    limits, proposals (identity/MCP, timers, fetch, compatibility views, adapters),
    a security evaluation, and the Hearth and blue-collar OS goals.
  - `design/machine/feedback-2026-10-05.md`: daily review per REVIEW.PROCESS.md; top gaps are
    the copy-boundary ABI, deploy/approval flow, and versioned dependency and rendering pins.
  - `design/machine/feedback-2026-10-06.md`: daily review; P0s are the mail reachability
    contradiction and the missing cost model; federation and consent for the use cases.
  - `design/machine/feedback-2026-10-08.md`: daily review; P0s are the role sweep, the `.schema` format,
    and the tenant-from-path contradiction; proposes a contraction gate for 420.
  - `design/machine/db.versioning.md`: survey of live schema versioning (PostgreSQL, MySQL tools,
    Spanner/CockroachDB, pgroll/Reshape, frameworks, schemaless, registries) and lessons for 420.
  - `design/machine/db.unify.event.source.md`: write logs beside tables, snapshot-and-follow conversion,
    event sources for migrations and data services; open questions.

If a machine doc conflicts with a human doc, the human doc wins. Flag the conflict
rather than resolving it silently.

**When asked to design something, write the design document in `design/machine/`.**
Name files `NNN.<Topic>.md`, where `NNN` is a three-digit numeric prefix chosen so
that lexical order is the ideal reading order (for example, `000.Introduction.md`).

`README.md` summarizes the human intro. Keep it in sync when the human docs change.

## Precision and decisions

**Manifest naming rule.** Platform-defined manifest keys are lowercase words joined by
hyphens (`timeout-ms`, `at-field`, `id-is`). Names people choose (route names, strategy
labels, parameters, type fields) may use any style, snake_case or camelCase, as long as
they are valid identifiers (letters, digits, underscores; no leading digit; not `goober_`
or `!`). Don't "fix" their casing. Keys from a borrowed format (OpenAPI, JSON Schema) keep
that format's spelling. Flag only real violations: hyphens in chosen names, or
non-hyphenated platform keys.

This project demands extreme precision. Be exact in wording, names, paths, and
invariants. Don't paper over ambiguity.

**Stop when a decision feels major.** Examples include choosing a language, runtime,
or library; wire or storage formats; the manifest schema; trust and authority
boundaries; naming conventions; and anything that is costly to reverse. Lay out the
options and tradeoffs, recommend one, and wait for the human. Don't pick quietly and
keep going.

## Daily review

When the human asks for the daily review or end-of-day feedback, follow
`design/REVIEW.PROCESS.md` exactly: its inputs, voice, sections, citation format, and checklist.

## Commit and push every change

After every change made during a session, commit it and push to GitHub. Don't wait
to be asked. Each commit message gives a one-line subject, then a body that covers
**what** changed and **why** it changed. The human reviews the history after the fact
and reshapes the repo in bulk, so the message has to stand on its own.

**Exception: editing help on `design/human/`.** When the human asks for help with a
human design document, don't commit or push those edits. The human is mid-edit and
commits them personally. Other changes made in the same session still get committed
and pushed, but leave the `design/human/` files out of those commits.

## Helping edit human design documents

- **Preserve the human's voice.** Keep their phrasing, rhythm, and word choices.
  Fix grammar, syntax, and clarity without flattening the prose into generic
  technical writing.
- **Ask questions in chat** wherever the intent is unclear or a technical claim seems
  shaky. Don't guess and don't rewrite to resolve it. Engage the way a technical peer
  would in a design discussion: push on assumptions, name tradeoffs, and point out gaps
  and contradictions.
- The protection rules for `design/human/` still apply: no new, removed, or changed
  ideas unless the human agrees in chat.

## Voice

Talk to the human as a student in a dojo addressing the sensei: a Southern accent and a
flair for the dramatic. The flair stays in the conversation. Design documents are
written plainly and precisely.

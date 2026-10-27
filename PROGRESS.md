# Progress

> The honest account of where goober is, kept by Claude and updated with every change.
> Started 2026-10-26, when the human said goober is now simply theirs: personal software
> they run for themselves and build products on. The human docs in `design/human/` are
> the intent. This file says how much of that intent the host in `host-src/` actually
> does, what is faked, and what comes next. Where it and the code disagree, the code
> wins and this file is wrong; please say so.

## In one paragraph

Goober today is a **single-machine host you can play with and run a personal site on**.
It serves a CMS (markdown, HTML, templates, listings, feeds, image attachments), runs
WASM boxes behind HTTP routes with grant-role requirements, gives boxes per-tenant
SQLite databases through checked SQL, signs people in with emailed codes, and is
operated by an agent over MCP. It is **not yet** the system the human docs describe:
there are no approvals, no write sets or outbox, no timers, no bus, no generated code,
no types, no domain registry, and no cluster. The mobile terminal (999) is the current
push, because it turns goober into something the human uses every day.

## Status by design document

Legend: **built** (does what the doc says, perhaps narrowed), **partial** (some of it,
listed), **none** (not started), **n/a** (philosophy, or not a host feature).

| doc | status | what exists | what's missing |
| --- | --- | --- | --- |
| 000 Introduction | partial | WASM boxes with no POSIX, manifests as perimeter, fuel and timeouts, SQLite storage, ephemeral solo boxes. | Shadow traffic, A/B, forking, per-box cost. |
| 005 Goals | partial | One binary on a laptop; Caddy in front for production (setup writes a Caddyfile); identity from day one. | Mail as a channel, the bus, timers, versions with approvers, the approval queue, worst-case answers from the manifest. |
| 010 Manifest | partial | `box`, `mission`, `domains`, `behavior` (timeout-ms, fuel, memory-max-bytes), `routes.http`, `templates`, `data`. Per-route exports (005 §3 supersedes signals). | `signal`, `response` kinds, mode/boundary/lifecycle beyond solo, budgets, shared isolates, `static`, `headers`, `rewrites`, internal routes, 4xx clamping. |
| 012 Configs | none | | Box-less config manifests, header priority, rewrites. |
| 013 Identity | partial | Realms (per tenant, not per domain), emailed codes, cookie sessions, host operators from config, first owner, requirements in DNF with substitution, operator grant/revoke, audit. **OAuth is being built now** (below). | Passwords, passkeys, box `grantable`/`revocable`, realms per primary domain, the operator dive handshake, new-session email. |
| 015 Domains | none | Domains live in the host config (005 §6). | The registry table, primaries and redirects, verification, TLS ask hook. |
| 018 Reserved | partial | `{!tenant}`, `{!user_id}` in requirements; `!` names refused. | `!domain`, `!email`, `!client_id`, and the rest; one-setter enforcement across protocols. |
| 019 Conflicts | partial | Overlapping routes refused at upload. | Traffic decisions, given-up routes, static and rewrite conflicts. |
| 020 DataFlow | partial | `data.sqlite3.$strategy` with `pattern: "sql"`, `{!tenant}` database names. | `by_id`, `by_index`, named writes, generated row structs, data signatures. |
| 021 PrimaryKeys | none | SQLite rowids (008 §9). | Host-minted ids, range leases. |
| 023 Sourcing | none | | Sources, `append`, event routes, cursors. |
| 025 DataShape | none | A SQLite transaction per database per request (008 §6). | Write sets, bundles, layered reads, leases, worst-case checks. |
| 028 Query | partial | Direct SQL under SQLite's authorizer, row limits, step budgets. | `pattern: "query"` iterators, plan checks, pagination cursors. |
| 030 Larger env. | none | Operators can fetch an https image with curl (Q21). | `external`, OpenAPI clients, `require`, credentials, egress audit. |
| 035 Outbox | none | | Outbox elements, dispatcher, idempotency keys, dead letters. |
| 036 Json | none | | The types document, the strict subset, validation, serialization. |
| 037 Http | partial | Typed path params, verbs, cross-site POST refusal, ETags on content. | Declared `query` and `request-headers`, `body` types, `max-body-bytes`, status vocabulary, CORS, ETags on box GETs, compression, ranges. |
| 038 Mail | none | A dev mailer that writes `.eml` files. | Mail routes, Stalwart, reply tokens, `send-email@1`, a real provider. |
| 039 Internal | none | | Internal routes, callers, context propagation. |
| 040 Bus | none | | Signals, `publish`, SSE subscriptions. |
| 041 Reserved routes | built | The reserved prefixes are refused, plus `/error-message/` and `/attachment/`. | `/internal/` hooks have nothing behind them yet; reserved mail addresses. |
| 045 Time | none | | Timer routes, `by_timer`, cron, DST rules. |
| 050 Agents/MCP | partial | The operator MCP endpoint (host:owner tools). | `/mcp` on every domain, box `mcp` routes, agent tokens, `"agents": true`, filtered discovery, agent audit. |
| 060 Files | partial | Attachments: sniffed types, a ledger, bytes through the WAL, an upload path (007 §7). | The `upload` verb on box routes, resizing and metadata stripping, `asset` responses, the sweeper. |
| 064 Keys | none | Secrets live in the host config file (mode 0600). | The vault, envelope encryption, `keys/...`. |
| 070 UI | partial | Markdown, Mustache with layouts, content negotiation for box JSON through templates, strict CSP on templated pages (007). | `static` in manifests, pins, the `/goober/` client script, sign-in through the product layout. |
| 099 The system | partial | Plain HTTP behind Caddy; systemd unit from setup. | Litestream, Stalwart, NATS, on-demand TLS ask. |
| 110 Languages | none | Boxes are written by hand in WAT or Rust against the JSON ABI. | C and Rust generators. |
| 120 Boundary | partial | Bounds-checked pointers, host-copied results; a JSON request document (005 §3). | Struct layouts, pickup and relocation, arenas. |
| 150 Deep manifest | partial | The fields above. Unknown fields refused, unbuilt known ones warned. | Most fields; deploy-time defaults and hash-named versions. |
| 300 Operating panel | partial | Access log, box logs, `host_health`, `access_log` and `box_logs` tools. | The panel UI, rollups, counters, traces, queues, client classes, user-data-free logs. |
| 305 Approvals | none | Dev policy: an upload that passes the checker goes live (005 §9). | The registry by hash, diffs, policies, modes. |
| 310 Deployments | partial | Versions kept; promote rolls back. | Shadowing, comparison reports, splitting. |
| 315 Static bundles | none | Content is written straight to disk. | Bundles, rogue-content checks. |
| 320 Working set | none | | Drafts for editors and agents. |
| 325 Managing keys | none | | |
| 330 Stability | none | | Capabilities, providers, pins, replay. |
| 390 Quotas | none | | |
| 400 Cluster | none | Single machine only. | |
| 410 API | partial | ~40 MCP tools: versions, content, pages, templates, attachments, schemas, databases, identity. | HTTP twins under `/goober-operations/`, host roles below owner, most catalog entries. |
| 420 Migration | partial | Expand-only schema application, refused changes named per tenant (008 §3). | Contraction, emulated type changes, upgrade boxes. |
| 999 Mobile | partial | **In progress**: see the plan. | Everything below the line in Phase 1. |

## Production readiness, honestly

Fine for **one person on a laptop**, and for a **personal VPS behind Caddy** with these
caveats, which Phase 3 works through:

- **Sign-in mail doesn't leave the machine.** The only mailer writes `.eml` files, so on
  a server you read your own code from `storage/mail/outbox/`. Nobody else can sign in.
- **No rate limits** on sign-in, tokens, or uploads beyond the per-address code throttle.
- **Secrets sit in the config file** (operator token, later mail credentials), protected
  only by file mode 0600. There is no vault (064).
- **Backups are manual**: copy `storage/` while the host is stopped. No Litestream, no
  online SQLite backup, no restore drill.
- **Logs grow forever** and the access log records paths, which can hold user data (300
  wants none).
- **No approvals**: whoever holds an operator token ships code live (005 §9).
- **Box isolation is wasmtime's** (fuel, epoch deadlines, memory caps, no imports beyond
  `goober`), which is sound; the host around it has had no outside security review.

## The plan

Ordered by what makes goober the human's own daily software first, then what makes it
safe to build products on. Checked items are done, tested, committed, and pushed.

### Phase 0: ingest 999 and tell the truth
- [x] Index `999.Mobile.md` in CLAUDE.md and README; fix the README's stale "no code yet".
- [x] Write this file.

### Phase 1: the mobile terminal (999)
Decisions taken provisionally from 999's own recommendations, recorded in
`design/machine/009.OAuth.md` and `010.Terminal.md`, each reversible:
1 clients say what they are, 2 loopback any port, 3 standard discovery and the fixed
client_id `dumb-terminal`, 4 `/terminal/v1/me`, 5 `minutes`, 6 product approvals only.
Decision 7 (push credentials) waits for the human; the terminal works by hourly pull.
- [ ] OAuth in the identity package: clients with `acts_as`, authorization codes with
      PKCE S256, rotating refresh tokens with reuse detection, revocation, grants.
- [ ] Host endpoints: `/.well-known/oauth-authorization-server`, `/oauth/authorize`
      (sign-in, then a consent screen that shows where the client comes from),
      `/oauth/token`, `/oauth/revoke`.
- [ ] Bearer access tokens on box routes and pages, resolved into the same caller as a
      cookie session; 401 with `WWW-Authenticate` and JSON for API callers.
- [ ] Agent tokens refused on HTTP routes unless the route says `"agents": true` (050).
- [ ] Clients from the host config (`identity.clients`), and operator tools
      `client_list`, `client_put`, `client_remove`.
- [ ] Dynamic registration and the device flow (`/oauth/register`, `/oauth/device`,
      `/oauth/device/verify`), agents only.
- [ ] The account page lists signed-in apps and agents, with revoke.
- [ ] The `terminal` box: me, config, sync, dashboards, decisions (idempotent by key,
      409 on a different key), devices, events.
- [ ] The `desk` box for agents: create approvals, read their outcome, set alarms and
      dashboards; no write path to an approval's status.
- [ ] An end-to-end script that plays the app against a running host.
- [ ] Try the real dumb app (desktop) against the demo host.

### Phase 2: agents as first-class callers (050)
- [ ] `/mcp` on every domain for agent tokens, serving box `routes.mcp` tools,
      discovery filtered by the person's roles, every call audited.
- [ ] Move the desk box's agent surface from `"agents": true` HTTP routes to MCP tools.

### Phase 3: production readiness
- [ ] A real mailer (SMTP submission with TLS), so others can sign in.
- [ ] Rate limits: sign-in starts, code checks, token and device polling, uploads.
- [ ] Online backups (SQLite backup API) and a tested restore; Litestream notes.
- [ ] Log rotation, and an access log without query strings or other user data.
- [ ] JSON error bodies for API callers; security headers everywhere.
- [ ] Graceful shutdown that drains requests; a health endpoint for Caddy.

### Phase 4: security review
- [ ] A written review (`design/machine/011.SecurityReview.md`): identity and OAuth,
      sessions and CSRF, content and XSS, attachments, SQL authorizer, WASM boundary,
      operator surface, filesystem paths, denial of service. Each finding fixed or
      recorded with a reason.

### Phase 5: the HTTP and JSON contract (036, 037)
- [ ] A `types` document (OpenAPI `components.schemas`, the strict subset) checked at
      upload; `body: "json:$Type"` validated before the box wakes, closed objects.
- [ ] Declared `query` and `request-headers`; `max-body-bytes`.
- [ ] ETags and 304 on box GETs.

### Phase 6: time (045)
- [ ] Timer routes with `every-ms` and `cron`, and data-driven `by_timer` rows with
      `timezone-field`, so "6am in each person's zone" can write the day's alarms.

### Phase 7: effects (035, 039) and push
- [ ] The outbox: elements captured with the request, delivered after commit, with
      idempotency keys and dead letters.
- [ ] Internal routes, so a decision can tell the agent's box.
- [ ] Push through FCM once decision 7 is made.

### Later, not planned yet
Domain registry (015), configs and rewrites (012), write sets and host ids (025, 021),
access patterns and generators (020, 028, 110, 120), sourcing (023), the bus (040),
mail routes (038), the upload verb (060), keys (064), the panel, approvals, deployments,
bundles, working sets (300 to 320), stability (330), quotas (390), the cluster (400),
and contraction (420). Each waits for the human to pull it forward.

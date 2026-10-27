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
| 013 Identity | partial | Realms (per tenant, not per domain), emailed codes, cookie sessions, host operators from config, first owner, requirements in DNF with substitution, operator grant/revoke, audit. OAuth 2.1 (009): PKCE codes with consent, rotating refresh tokens with reuse detection, revocation, dynamic registration, the device flow, bearer tokens, person and agent clients, signed-in apps on the account page. | Passwords, passkeys, box `grantable`/`revocable`, realms per primary domain, the operator dive handshake, new-session email, rate limits on the token endpoints. |
| 015 Domains | none | Domains live in the host config (005 §6). | The registry table, primaries and redirects, verification, TLS ask hook. |
| 018 Reserved | partial | `{!tenant}`, `{!user_id}` in requirements; `!` names refused. | `!domain`, `!email`, `!client_id`, and the rest; one-setter enforcement across protocols. |
| 019 Conflicts | partial | Overlapping routes refused at upload. | Traffic decisions, given-up routes, static and rewrite conflicts. |
| 020 DataFlow | partial | `data.sqlite3.$strategy` with `pattern: "sql"`, `{!tenant}` database names. | `by_id`, `by_index`, named writes, generated row structs, data signatures. |
| 021 PrimaryKeys | none | SQLite rowids (008 §9). | Host-minted ids, range leases. |
| 023 Sourcing | none | | Sources, `append`, event routes, cursors. |
| 025 DataShape | none | A SQLite transaction per database per request (008 §6). | Write sets, bundles, layered reads, leases, worst-case checks. |
| 028 Query | partial | Direct SQL under SQLite's authorizer, row limits, step budgets. | `pattern: "query"` iterators, plan checks, pagination cursors. |
| 030 Larger env. | partial | `external` services by verb and path, `require` pins from the context, credentials by key name, per-request budgets, an egress log (015). | OpenAPI documents and generated clients, async calls, per-service rate limits, box-to-box calls. |
| 035 Outbox | partial | Elements committed with the request's writes, a dispatcher with ordered delivery per service, backoff, stable idempotency keys, dead letters and retry (015). | Result routes, delivery to internal routes. |
| 036 Json | partial | The types document in the strict subset (plus `date-time` and `date`), stored with the version, used to check bodies, queries, tool inputs, and typed responses; int64 as strings; defaults filled (011). | `token`, change classification, generated structures and serializers. |
| 037 Http | partial | Typed path params, verbs, cross-site POST refusal, ETags on content; declared `body`, `max-body-bytes`, `query`, `request-headers`, and typed `response`, checked by the host (011). | The status vocabulary, CORS, ETags on box GETs, compression, ranges; undeclared routes still see every header and parameter. |
| 038 Mail | none | A dev mailer that writes `.eml` files. | Mail routes, Stalwart, reply tokens, `send-email@1`, a real provider. |
| 039 Internal | partial | Internal routes with callers, synchronous calls carrying the person and tenant, both sides checked, cycles refused, depth capped, deadlines shrink (016). | Outbox to boxes, idempotent routes, typed generated calls, tenant binding by path, worst-case sums. |
| 040 Bus | none | | Signals, `publish`, SSE subscriptions. |
| 041 Reserved routes | built | The reserved prefixes are refused, plus `/error-message/` and `/attachment/`. | `/internal/` hooks have nothing behind them yet; reserved mail addresses. |
| 045 Time | partial | Timer routes (every-ms, cron), `by_timer` sources with per-row zones and one-shot at-fields, 045's DST rules, catch-up, operator tools (014). | Spreading fires, `target` to internal routes, `tenant-field`, typed row structs, pause and resume. |
| 050 Agents/MCP | partial | The operator MCP endpoint. Agent tokens (009), refused on HTTP routes unless `"agents": true`. `/mcp` on every domain with box tools, typed inputs, filtered discovery, an audit log, and RFC 9728 metadata (012). | Resources, subscriptions, sessions and cancellation, per-agent quotas, the agent history on the account page, a run with a real MCP client. |
| 060 Files | partial | Attachments: sniffed types, a ledger, bytes through the WAL, an upload path (007 §7). | The `upload` verb on box routes, resizing and metadata stripping, `asset` responses, the sweeper. |
| 064 Keys | partial | `keys/system/...` and `keys/tenant/{!tenant}/...`, set write-only by operators, attached by the host at call time (015). | The vault, envelope encryption, rotation, user keys, the key screen for people. |
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
| 999 Mobile | partial | OAuth as a person client (009); the `terminal` and `desk` boxes (010): me, config, sync, dashboards, idempotent decisions, devices, events; approvals, alarms, dashboards, and settings from agents, with no write path to decisions; checked end to end. Local mode answers the emulator's 10.0.2.2. | Push (decision 7), the outbox to tell agents, the 6am timer, MCP tools for agents, host-validated types, ETags on the snapshot, a run with the real app. |

## Production readiness, honestly

Fine for **one person on a laptop**, and for a **personal VPS behind Caddy** with these
caveats, which Phase 3 works through:

- **Sign-in mail** goes through SMTP when `identity.mailer` is `smtp`; otherwise the dev
  mailer writes `.eml` files and nobody else can sign in. The SMTP path is tested against
  a stand-in curl, not yet against a real mail server.
- **Rate limits** are per client address and in memory: they reset on restart, and a
  client with many addresses gets many budgets.
- **Secrets sit in files** protected only by mode 0600: the operator token in the config,
  the SMTP password in its own file. There is no vault (064).
- **Backups are on demand**: `just backup DEST` takes a consistent copy while the host runs,
  and restore is tested, but nothing schedules them and there is no Litestream yet.
- **Logs rotate** at 64 MiB (three kept), but the access log still records paths, which can
  hold user data (300 wants none).
- **No approvals**: whoever holds an operator token ships code live (005 §9).
- **Box isolation is wasmtime's** (fuel, epoch deadlines, one memory and one bounded table,
  no imports beyond `goober`). The host around it has had one internal review
  (`013.SecurityReview.md`) and no outside one. The identity screens still share an origin
  with box pages (Q24).

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
- [x] OAuth in the identity package: clients with `acts_as`, authorization codes with
      PKCE S256, rotating refresh tokens with reuse detection, revocation, grants.
- [x] Host endpoints: `/.well-known/oauth-authorization-server`, `/oauth/authorize`
      (sign-in, then a consent screen that shows where the client comes from),
      `/oauth/token`, `/oauth/revoke`.
- [x] Bearer access tokens on box routes and pages, resolved into the same caller as a
      cookie session; 401 with `WWW-Authenticate` and JSON for API callers.
- [x] Agent tokens refused on HTTP routes unless the route says `"agents": true` (050).
- [x] Clients from the host config (`identity.clients`), and operator tools
      `client_list`, `client_put`, `client_remove`.
- [x] Dynamic registration and the device flow (`/oauth/register`, `/oauth/device`,
      `/oauth/device/verify`), agents only. See `design/machine/009.OAuth.md`.
- [x] The account page lists signed-in apps and agents, with revoke.
- [x] The `terminal` box: me, config, sync, dashboards, decisions (idempotent by key,
      409 on a different key), devices, events.
- [x] The `desk` box for agents: create approvals, read their outcome, set alarms and
      dashboards; no write path to an approval's status.
- [x] An end-to-end script that plays the app against a running host (`just terminal-e2e`,
      48 checks). See `design/machine/010.Terminal.md`.
- [ ] Try the real dumb app against the demo host. This needs the human: sign-in happens in a
      real browser. The app's own e2e test is tied to its dev server's `/oauth/consent`
      shortcut, so it can't drive goober's consent screen unchanged.

### Phase 2: agents as first-class callers (050), and the types they need (036, 037)
- [x] The `types` package: a box's OpenAPI types document in 036's strict subset, stored
      with the version, checked at upload (`design/machine/011.Types.md`).
- [x] Routes declare `body`, `max-body-bytes`, `query`, `request-headers`, and `response`;
      the host checks requests before the box wakes and typed responses before they leave.
- [x] `/mcp` on every domain for bearer tokens, serving box `routes.mcp` tools, discovery
      filtered by the person's roles, inputs typed, every call audited in `logs/agents.log`;
      RFC 9728 protected-resource metadata so MCP clients find the realm
      (`design/machine/012.Agents.md`).
- [x] The desk box offers ten MCP tools and typed HTTP bodies; the end-to-end script asks
      over MCP and answers on the terminal (55 checks).
- [ ] Connect a real MCP client (Claude Code or Grok) to `/mcp` through OAuth.

### Phase 3: production readiness
- [x] A real mailer: SMTP with TLS through curl, from a background queue so timing reveals
      nothing, the password read from a file and kept off the command line; setup asks.
- [x] Rate limits per client address: one budget for everything (`limits.requests-per-minute`)
      and tighter ones for sign-in, tokens, registration, the device flow, consent, uploads,
      and `/mcp`; `listen.behind-proxy` reads the address from X-Forwarded-For behind Caddy.
- [x] Online backups (`just backup DEST`, SQLite's backup API, safe while running) and a
      tested restore into empty storage (`just restore FROM`). Litestream is still to come.
- [x] Logs rotate at 64 MiB keeping three; the access log records paths (capped), never
      query strings or tokens. (Paths themselves can still hold user data.)
- [x] JSON error bodies for API callers; HSTS on TLS; strict Host headers; the templated
      CSP on box pages; no CR/LF in any header; a private umask for everything stored.
- [x] Graceful shutdown: stop accepting, finish in-flight requests (up to 10 s), then close;
      `/goober/health` for Caddy, which setup's Caddyfile now checks.

### Phase 4: security review
- [x] Three read-only reviews (identity and OAuth; content and files; the box boundary,
      SQL, operators, and HTTP), every confirmed finding fixed with a test or recorded with
      a reason: `design/machine/013.SecurityReview.md`. Two findings let one box crash the
      whole host; both are fixed.
- [ ] A separate origin for the identity and consent screens (Q24), the one finding whose
      full fix is a trust-boundary decision for the human.
- [ ] Fuzz the parsers (markdown, Mustache, front matter, types, multipart, SQL tokenizer).

### Phase 5: the rest of the HTTP contract (037)
- [ ] ETags and 304 on box GETs.
- [ ] The status vocabulary: clamp box 4xx, tie success codes to the response kind.
- [ ] CORS declared in the manifest, preflight answered by the host.

### Phase 6: time (045)
- [x] Timer routes with `every-ms` and `cron`, and data-driven `by_timer` sources firing per
      row in each row's zone or once at a row's `at-field` (turned off with the fire's own
      writes); catch-up after downtime; `timers_list` and `timer_run`
      (`design/machine/014.Time.md`).
- [x] The desk's 6am briefing: in each device's zone, "N approvals are waiting for you."

### Phase 7: effects (030, 035, 039) and push
- [x] External services (`external`, `goober_call`): safe calls now, effectful ones through an
      outbox committed with the request's writes, delivered in order with retries, one
      idempotency key, dead letters, and operator tools; a write-only key store
      (`design/machine/015.Network.md`).
- [x] Internal routes (039): `routes.internal` with callers, called synchronously through
      `goober_call` as the same person, both sides checked and cycles refused at upload
      (`design/machine/016.Internal.md`).
- [ ] Outbox elements addressed to a box, idempotent internal routes, and timer targets.
- [ ] Outbox result routes (035), so a box hears how its delivery went.
- [ ] Push through FCM once decision 7 is made (Q26).

### Building products
- [x] `box_guide` on the operator MCP: the boundary, manifest, routes, tools, timers, data,
      and types in one read, so an agent can build and deploy a product from it.

### Later, not planned yet
Domain registry (015), configs and rewrites (012), write sets and host ids (025, 021),
access patterns and generators (020, 028, 110, 120), sourcing (023), the bus (040),
mail routes (038), the upload verb (060), keys (064), the panel, approvals, deployments,
bundles, working sets (300 to 320), stability (330), quotas (390), the cluster (400),
and contraction (420). Each waits for the human to pull it forward.

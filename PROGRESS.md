# Progress

> The honest account of where goober is, kept by Claude and updated with every change.
> Started 2026-10-26, when the human said goober is now simply theirs: personal software
> they run for themselves and build products on. The human docs in `design/human/` are
> the intent. This file says how much of that intent the host in `host-src/` actually
> does, what is faked, and what comes next. Where it and the code disagree, the code
> wins and this file is wrong; please say so.

## In one paragraph

Goober today is a **single-machine host you can run your own life and products on**,
alone or with a few people. It serves a CMS (markdown, HTML, templates, listings, feeds,
attachments); runs WASM boxes behind HTTP routes, MCP tools for agents, timers, and
internal routes; gives boxes per-tenant SQLite through checked SQL and typed requests;
lets boxes call outside services with keys they never see, through an outbox that only
fires if the request commits; reads the web politely for operators, crawls, and boxes,
turning pages into text, markdown, links, schema.org data, and scraped records; signs
people in with emailed codes or OAuth (the dumb terminal, agents through the device flow,
MCP clients, or long-lived minted tokens); sends mail through Amazon SES or SMTP, boxes' mail
included; keeps agents' source code itself, building Rust in a sandbox and deploying from a
revision; and is operated by an agent over MCP with 70 tools. It deploys to a fresh server
with one command and backs itself up to S3: sealed nightlies kept 14 days, database
increments every 15 minutes, and attachments, any moment of which rehydrates into a
sandboxed devbox. It has had five rounds of internal review and fuzzing. Two example products run on it end to end:
an ad landing page that collects emails and surveys, and a ranch CRM and meat index fed by
the crawler. `READINESS.md` says what that means for the human's experiments. It is **not yet** the system the human docs describe: there are
no approvals, no host write sets, no bus, no generated code, no domain registry, no
mail routes, and no cluster, and a few things only the human can do: run the real dumb app,
connect a real MCP client, try real AWS, and decide the open questions (Q17 to Q38).

## Status by design document

Legend: **built** (does what the doc says, perhaps narrowed), **partial** (some of it,
listed), **none** (not started), **n/a** (philosophy, or not a host feature).

| doc | status | what exists | what's missing |
| --- | --- | --- | --- |
| 000 Introduction | partial | WASM boxes with no POSIX, manifests as perimeter, fuel and timeouts, SQLite storage, ephemeral solo boxes. | Shadow traffic, A/B, forking, per-box cost. |
| 005 Goals | partial | One binary on a laptop; Caddy in front for production (setup writes a Caddyfile); identity from day one. | Mail as a channel, the bus, timers, versions with approvers, the approval queue, worst-case answers from the manifest. |
| 010 Manifest | partial | `box`, `mission`, `domains`, `behavior` (timeout-ms, fuel, memory-max-bytes), `routes.http`, `routes.mcp`, `routes.timer`, `routes.internal`, `routes.identity` (021), `templates`, `types`, `data`, `external`. Per-route exports (005 §3 supersedes signals). 5xx is the host's (011). | `signal`, mode/boundary/lifecycle beyond solo, budgets, shared isolates, `static`, `headers`, `rewrites`, 4xx clamping. |
| 012 Configs | none | | Box-less config manifests, header priority, rewrites. |
| 013 Identity | partial | Realms (per tenant, not per domain), emailed codes, cookie sessions, host operators from config, first owner, requirements in DNF with substitution, operator grant/revoke, audit. OAuth 2.1 (009): PKCE codes with consent, rotating refresh tokens with reuse detection, revocation, dynamic registration, the device flow, bearer tokens, person and agent clients, signed-in apps on the account page. Addresses proved before an account stays (invites expire in 30 days), and sign-in events boxes hear (021). | Passwords, passkeys, box `grantable`/`revocable`, realms per primary domain, the operator dive handshake, a separate origin for the screens (Q24). Rate limits, new-grant email, and browser sessions on the account page are built (013 review). |
| 015 Domains | partial | Domains live in the host config (005 §6): a tenant's, or a redirect the host answers with a 301 to its primary, keeping the path. | The registry table, one primary per tenant enforced, verification, TLS ask hook. |
| 018 Reserved | partial | `{!tenant}`, `{!user_id}` in requirements; `{!email}`, `{!user_id}`, `{!tenant}`, `{!domain}` in `require`; `client_id` for agents and a `caller` block for internal calls in the request document; `!` names refused. | Substituting `!client_id` and the rest in requirements; one-setter enforcement across protocols. |
| 019 Conflicts | partial | Overlapping routes refused at upload; across boxes a literal beats a parameter; `route_check` names what answers a path and what it shadows. | Traffic decisions, given-up routes, static and rewrite conflicts. |
| 020 DataFlow | partial | `data.sqlite3.$strategy` with `pattern: "sql"`, `{!tenant}` database names, a fixed `tenant` per strategy so one database spans domains, and boxes sharing databases with their own grants, their schemas checked to fit (008 §4). | `by_id`, `by_index`, named writes, generated row structs, data signatures. |
| 021 PrimaryKeys | partial | One id service (038): ranges vended durably, users, invites, uploads, and keys of type `id` stamped by the host and refused when a box names one; ids as strings; restores jump past. | Other host tables' ids; per-thread slices; a cluster's counter. |
| 023 Sourcing | none | | Sources, `append`, event routes, cursors. |
| 025 DataShape | none | A SQLite transaction per database per request (008 §6). | Write sets, bundles, layered reads, leases, worst-case checks. |
| 028 Query | partial | Direct SQL under SQLite's authorizer, row limits, step budgets; grants are a box's whole reach, and `grant_usage` shows what each is used for (008 § 4.1). | `pattern: "query"` iterators, plan checks, pagination cursors. |
| 030 Larger env. | partial | `external` services by verb and path, `require` pins from the context, credentials by key name, per-request budgets, never past the request's deadline, an egress log (015); box-to-box calls (016); the web as a service through the host's robot (017). | OpenAPI documents and generated clients, async calls, per-service rate limits. |
| 035 Outbox | partial | Elements committed with the request's writes, a dispatcher delivering each service from its own oldest element (one stuck service never blocks another), backoff, stable idempotency keys, dead letters and retry, and stranded outboxes found by scanning storage (015). | Result routes, delivery to internal routes. |
| 036 Json | partial | The types document in the strict subset (plus `date-time` and `date`), stored with the version, used to check bodies, queries, tool inputs, and typed responses; int64 as strings; defaults filled (011). | `token`, change classification, generated structures and serializers. |
| 037 Http | partial | Typed path params, verbs, cross-site POST refusal, ETags on content and box GETs; declared `body`, `max-body-bytes`, `query`, `request-headers`, and typed `response`; the status vocabulary; CORS per route (011). | ETags that skip the box, compression, ranges, declared redirect targets; undeclared routes still see every header and parameter. |
| 038 Mail | partial | Mail in through Stalwart and `routes.mail`, with reply tokens and the box's answer as the SMTP answer (024). Mail out through SMTP or Amazon SES; boxes send through a host `mail` service, via the outbox, within per-tenant and per-address daily limits; a sandbox sends nothing (018). | Verified senders (Q39), verified senders per domain for mail out, `send-email@1`, bounce handling. |
| 039 Internal | partial | Internal routes with callers, synchronous calls carrying the person and tenant, both sides checked, cycles refused, depth capped, deadlines shrink (016). | Outbox to boxes, idempotent routes, typed generated calls, tenant binding by path, worst-case sums. |
| 040 Bus | partial | In-process (036): `publish` on strategies, moved on commit; `routes.sse` subscriptions evaluated read-only, coalesced, shared per signature, refreshed, over SSE with keepalive and a 15-minute life; `bus_status`. | A cluster (NATS), publishing from operator SQL, topics from route parameters. |
| 041 Reserved routes | built | The reserved prefixes are refused, plus `/error-message/` and `/attachment/`. | `/internal/` hooks have nothing behind them yet; reserved mail addresses. |
| 045 Time | partial | Timer routes (every-ms, cron), `by_timer` sources with per-row zones and one-shot at-fields, 045's DST rules, catch-up, operator tools (014). | Spreading fires, `target` to internal routes, `tenant-field`, typed row structs, pause and resume. |
| 050 Agents/MCP | partial | The operator MCP endpoint. Agent tokens (009), refused on HTTP routes unless `"agents": true`. `/mcp` on every domain with box tools, typed inputs, filtered discovery, an audit log, and RFC 9728 metadata (012). | Resources, subscriptions, sessions and cancellation, per-agent quotas, a run with a real MCP client. |
| 060 Files | partial | Attachments: sniffed types, a ledger, bytes through the WAL, an upload path (007 §7). | The `upload` verb on box routes, resizing and metadata stripping, `asset` responses, the sweeper. |
| 064 Keys | partial | `keys/system/...` and `keys/tenant/{!tenant}/...`, set write-only by operators, each bound to the hosts it may be sent to, attached by the host at call time (015); the host's own AWS and backup keys can be set only on the machine (018, 019). | The vault, envelope encryption, rotation, user keys, the key screen for people. |
| 070 UI | partial | Markdown, Mustache with layouts, content negotiation for box JSON through templates, strict CSP on templated pages (007). | `static` in manifests, pins, the `/goober/` client script, sign-in through the product layout. |
| 099 The system | partial | Plain HTTP behind Caddy; systemd unit from setup. | Litestream, Stalwart, NATS, on-demand TLS ask. |
| 110 Languages | none | Boxes are written by hand in WAT or Rust against the JSON ABI. | C and Rust generators. |
| 120 Boundary | partial | Bounds-checked pointers, host-copied results; a JSON request document (005 §3). | Struct layouts, pickup and relocation, arenas. |
| 150 Deep manifest | partial | The fields above. Unknown fields refused, unbuilt known ones warned. | Most fields; deploy-time defaults and hash-named versions. |
| 300 Operating panel | partial | Access log, box logs, `host_health`, `access_log` and `box_logs` tools. The panel (028): its own origin (`operations.domain` or `operations.listen`), host-role sign-in, look-only; traffic rollups from the access log (minutes 48 h, hours 400 days), the heat map, data-flow graphs, the change log from the WAL, operator actions, and a read-only database explorer; `host:observer` read-only over MCP too. | Approvals, tuning instances and queues, counters, traces, cost, memory and queue depth, client classes, `principal_kind`. |
| 305 Approvals | none | Dev policy: an upload that passes the checker goes live (005 §9). | The registry by hash, diffs, policies, modes. |
| 310 Deployments | partial | Versions kept; promote rolls back. | Shadowing, comparison reports, splitting. |
| 315 Static bundles | none | Content is written straight to disk. | Bundles, rogue-content checks. |
| 320 Working set | none | | Drafts for editors and agents. |
| 325 Managing keys | none | | |
| 330 Stability | none | | Capabilities, providers, pins, replay. |
| 390 Quotas | none | | |
| 400 Cluster | none | Single machine only. | |
| 410 API | partial | 70 MCP tools: versions, content, pages, templates, attachments, schemas, databases, identity and clients (and minted agent tokens), timers, keys, the outbox, the web and crawls, backups, source projects, the Rust binding, and the site and box guides. | HTTP twins under `/goober-operations/`, host roles below owner, most catalog entries. |
| 420 Migration | partial | Expand-only schema application, refused changes named per tenant (008 §3). | Contraction, emulated type changes, upgrade boxes. |
| 521 Source | built | Drafted by Claude at the human's request, uncommitted for the human. Projects with numbered revisions, content stored once, commits that land on top when they don't overlap, diffs, revert; builds of Rust (in a bubblewrap sandbox, offline, vendored crates), WAT, or prebuilt WASM; deploys of schemas, boxes, and sites; eleven MCP tools and `goober.py source` (020). | Branches, merges inside a file, GitHub mirroring, deleting projects, crates beyond the vendored set. |
| 663 Backups | built | Drafted by Claude at the human's request, uncommitted for the human. Sealed (ChaCha20 + HMAC) nightlies kept keep-days by timestamp, page-level database increments, attachments queued once each, rotation, `backup_status`, and `goober-host rehydrate` into a sandboxed devbox (019). | A panel page, multipart uploads past 5 GiB, two destinations, key rotation, restoring into a live host. |
| 800 Crawl | partial | Drafted by Claude at the human's request, uncommitted for the human (Q32). The robot: robots.txt, pacing per site, public addresses only (pinned through redirects), a cache with conditional refetches, opt-out; views (html, text, markdown, links, meta with the shop platform, JSON-LD, tables, select, extract); operator crawls with scope, sitemaps, and records; a `web` service for boxes; the ranches example (017). | JavaScript rendering, a public opt-out page, sharing crawl results between boxes, robots and pacing state across restarts. |
| 999 Mobile | partial | OAuth as a person client (009); the `terminal` and `desk` boxes (010): me, config, sync, dashboards, idempotent decisions, devices, events; approvals, alarms, dashboards, and settings from agents over HTTP or ten MCP tools, typed by the host, with no write path to decisions; a 6am briefing timer in each device's zone (014); ETags on the snapshot; 57 end-to-end checks. Local mode answers the emulator's 10.0.2.2. | Push (decision 7), telling an agent's box about a decision (agents poll), a run with the real app. |

## Production readiness, honestly

Fine for **one person on a laptop**, and for a **personal VPS behind Caddy**
(`just deploy you@server [--without-rust]`: Ubuntu 24.04+ or Debian 13+, a `goober` user,
systemd, bubblewrap, local nightly copies, and S3 backups when setup is given a bucket;
written and dry-run here, not yet run against a real server), with these caveats:

- **Mail** goes through SES (`identity.mailer: "ses"`) or SMTP; otherwise the dev mailer
  writes `.eml` files and nobody else can sign in. SES and S3 are tested against stand-ins
  that check AWS signatures, and the signer against AWS's published examples and curl, but
  not yet against real AWS. A new SES account starts in its sandbox.
- **Rate limits** are per client address and in memory: they reset on restart, and a
  client with many addresses gets many budgets.
- **Secrets sit in files** protected only by mode 0600: the operator token in the config,
  the SMTP password in its own file. There is no vault (064).
- **Backups go to S3** when `backup.s3` is set: sealed with a backup key that must also be
  kept off the machine (`goober-host backup key`), or nothing can be restored. Rehydrate is
  tested end to end against a stand-in S3. Without a bucket, backups are local copies only.
- **Logs rotate** at 64 MiB (three kept), but the access log still records paths, which can
  hold user data (300 wants none).
- **No approvals**: whoever holds an operator token ships code live (005 §9).
- **Box isolation is wasmtime's** (fuel, epoch deadlines, one memory and one bounded table,
  no imports beyond `goober`). The host around it has had one internal review, fuzzing,
  and a sanitizer run (`013.SecurityReview.md`), and no outside review. The identity screens
  still share an origin with box pages (Q24).
- **Keys** for outside services are plain 0600 files, write-only through the tools, and
  each goes only to the hosts it was set for.
- **Source builds** run cargo and build scripts from vendored crates inside bubblewrap (no
  network, no secrets); bubblewrap on Ubuntu 24.04's AppArmor rules is untested.
- **Minted agent tokens** live up to a year (Q31): treat one like a password, and revoke it
  from the account page when an agent is done.
- **The robot** reads only public addresses, obeys robots.txt, and paces itself, but what a
  product republishes from other sites is the human's call (Q33).

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
      now 57 checks with MCP and the briefing). See `design/machine/010.Terminal.md`.
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
- [x] The account page shows what the person's agents did (the newest 20 tool calls).
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
- [x] Fuzz the parsers and property-test the SQL authorizer (`just fuzz`, under ASan and
      UBSan): every parser clean; two authorizer holes found and fixed (013 F1, F2).
- [x] Fuzz the multipart parser and the HTTP server (raw mutated requests over sockets; the
      server stays up and answers afterwards). Multipart boundaries are capped at 70 characters.
- [x] A second review (2026-10-27): correctness of the network, outbox, scheduler, keys, mail,
      and shutdown code, and product readiness; twelve correctness findings and three deploy
      blockers fixed (013 R1 to R12, P1 to P3), among them a `..` path value walking out of
      an external call's declared path with its credential attached.
- [x] A third review, of the robot and the example products: eleven findings fixed (013 W1 to
      W11), among them a robots.txt that could hold a thread for 23 s and a cache a box could
      read past its own allow list. The whole suite also passes under ASan and UBSan.
- [x] A fourth review (2026-10-27, evening), security and code quality across the host: every
      finding fixed or recorded (013 S4-1 to S4-12, Q4-1 to Q4-14), among them nested
      templates that could crash the host, a box sending a stored key to its own server,
      services reaching private addresses, timers firing another box's rows, and a
      multi-database commit that could enqueue mail for writes that didn't land.
- [x] A fifth review (2026-10-28), of the backup, source, and mail code: twelve findings fixed
      (013 V1 to V12), the worst a build script that could make the host read a key file and
      echo it into a job log. The suite (260 tests) passes under ASan and UBSan.

### Phase 5: the rest of the HTTP contract (037)
- [x] ETags and 304 on box GETs (from the answer's bytes; the box still runs).
- [x] The status vocabulary: 5xx is the host's, redirects only to same-site paths, `empty` is 204.
- [x] CORS per route (`"cors"`), preflights answered by the host, never with credentials.
- [ ] ETags from commit versions, so a 304 doesn't wake the box; compression and ranges.

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
- [x] Outbox result routes (035), so a box hears how its delivery went: `result-route` on a
      method, `routes.result`, and `delivery.timeout-ms` up to two minutes for model APIs.
- [ ] Push through FCM once decision 7 is made (Q26).

### Building products
- [x] `box_guide` on the operator MCP: the boundary, manifest, routes, tools, timers, data,
      and types in one read, so an agent can build and deploy a product from it.
- [x] Landing pages and forms: plain HTML forms arrive decoded as `form`, every request has
      a pseudonymous `client_key`, anonymous writes have their own budget, and static site
      generator output (`_astro/`, `_next/`) is served.
- [x] Agents that can't do OAuth: `agent_token_mint` (Q31); `rust_binding` hands a remote
      agent the Rust binding; operators may push 64 MiB bodies once their token checks out.
- [x] Boxes share databases, across domains with a fixed `tenant` (008 §4, Q29, Q30).
- [x] The `leads` example: an ad's landing page collecting emails, consent, surveys, and UTM
      tags; optional confirmation mail through the outbox; tools to count, tally, and export.
- [x] The `ranches` example: a ranch CRM with an outreach log and a meat index read by the
      robot; public `/meat/offerings`. `just products-e2e` plays both as GrokBot (30 checks).
- [ ] Run a real experiment: a real domain, a real mail provider, a real ad, and Grok
      connected over its remote MCP tool. This needs the human.

### Phase 8: deploying (099)
- [x] A `server` build preset: one binary, libstdc++ linked in, needing only glibc 2.38.
- [x] `just deploy you@server`: build here, install there (Caddy, a `goober` user, setup,
      systemd, nightly backups), and upgrade on later runs. Caddy's health check works on
      any Host. Dry-run here; not yet run against a real server.
- [ ] Run it against the human's new machine.

### Phase 9: the robot (800)
- [x] The `web` package: URLs, robots.txt, a tolerant HTML parser, and the views, fuzzed.
- [x] The robot in the host, operator `web_fetch` and crawls, and the `web` service for boxes
      (`design/machine/017.Crawl.md`).
- [ ] Robots and pacing state that survives a restart; a public opt-out page (Q32).

### Phase 10: mail and backups (018, 663)
- [x] The `aws` package: SigV4 (AWS's examples and curl agree), S3, and SES.
- [x] SES as the host's mailer; a `mail` service for boxes through the outbox, with limits;
      the `sandbox` flag (`design/machine/018.Mail.md`).
- [x] Backups to S3: sealed nightlies with rotation, page-level increments, queued
      attachments; `goober-host rehydrate` and `just rehydrate`; `just backup-e2e`
      (`design/machine/019.Backups.md`).
- [ ] Run against real AWS: a verified SES domain out of the sandbox, a bucket, and a
      rehydrate on the human's laptop. This needs the human.

### Phase 11: source kept in the host (521)
- [x] The `source` package and the operator tools: create, checkout, commit, log, diff,
      revert, build, deploy, jobs (`design/machine/020.Source.md`).
- [x] Sandboxed Rust builds (bubblewrap, offline, vendored crates), WAT, and prebuilt WASM;
      Rust on every deploy (`--without-rust` to skip); `goober.py source` for people.
- [x] Deploys install a C toolchain for crates' build scripts, and end by checking everything:
      services, timers, `goober-host toolchain` (a real sandboxed Rust build with a derive),
      Stalwart on port 25 refusing an unserved address, and the mail hook; a failed check fails
      the deploy.
- [ ] Grok driving the loop over its remote MCP tool. This needs the human.

### Phase 12: identity events and mail templates (013, 038)
- [x] Accounts exist once an address is proved; invites never taken up expire in 30 days;
      sign-ins append `first-verified` and `signed-in` events; boxes hear them through
      `routes.identity`, after commit, in order, with retries (`design/machine/021.IdentityEvents.md`).
- [x] A site's `/mcp` with no tools for the caller says where the operator endpoint is.
- [x] Mail templates for every mail, with defaults (a welcome for new addresses, a leaner one
      for returning), per-tenant and host-wide overrides, a layout, HTML with a text part,
      operator tools, and boxes sending their site's own templates (`design/machine/022.MailTemplates.md`).
- [x] Operator OAuth: `/oauth/operator`, an issuer in the host realm that only operators sign
      in to, advertised by `/goober-operations-mcp`'s 401, so agents whose connectors only
      speak OAuth (Grok) reach the operator tools (`design/machine/023.OperatorOAuth.md`).
- [ ] Grok connecting through it for real. This needs the human.

### Phase 13: mail in (038)
- [x] `routes.mail.$address.receive` (exact, local@ on every domain, catch-all `*@`, params,
      `{name : token}`), one box per address per domain; the MTA hook at
      `/internal/routes/mta/stalwart` (loopback, no proxy headers, a bearer token from
      `mail.inbound.token-file`); RCPT refused at the door, one recipient per message; DATA
      parsed (MIME, charsets, attachments kept in the host), automatic mail dropped unless a
      route opts in, the box's answer as the SMTP answer (250, 550, 451); signed reply
      addresses minted by `send` with `reply`; attachments read by handle.
- [x] Stalwart 1.0.1 (pinned, checksummed) installed or upgraded and configured by every deploy
      (`--without-mail` skips), never able to relay or queue, with Caddy's certificate and the
      MX records printed (`tools/stalwart.sh`); `just stalwart-e2e` and `just mail-e2e` run the
      real Stalwart, the second against a real host and box, over SMTP.
- [ ] Verified senders (038 § The sender is a verified claim): Stalwart's hook carries no SPF,
      DKIM, or DMARC result, so `sender` is null (Q39).
- [ ] Mail reaching the real server: port 25 open, MX records, a first message. This needs the human.

### Phase 14: telephony (designed, waiting on the human)
Proposed in `design/machine/025.Telephony.md`: a number per tenant from a rented provider,
`routes.phone` for texts and calls in, a `phone` service for texts out, host-config grants
that upload checks, `host:owner`-only spending, STOP/HELP and opt-outs in the host, and a
spend cap. Nothing is built until Q42 to Q49 are answered.
- [ ] The human ratifies 025 and picks a provider (Q42) and a first path (Q43).

### Phase 15: the email worker (026)
- [x] Outreach mail: a service's own `from`, threading headers, one-click unsubscribe and a
      per-tenant list (018 § 3).
- [x] Result routes and two-minute deliveries, for model calls (015 § 3).
- [x] Replies in a thread the person started get their own daily allowance.
- [x] Host-built JSON-LD for box templates (007 § 8).
- [ ] M1: Stalwart's DMARC strict, once real mail arrives (Q39).
- [ ] M2: SES bounces and complaints through SNS, unsubscribing hard bounces and complaints.
- [x] M3: `mail_activity`, a tenant's mail in and out for operators and agents.
- [x] M4: attachments on `send` (783).
- [x] M5: per-tenant daily caps on outbox calls per service (`limits.outbox-daily-per-service`).
- [x] M6: markdown in `send` (783).

### Phase 16: shared partials (027)
- [x] `_partials/$name.html` included by `<!--#include name-->` in pages, box templates, and box
      HTML with `"includes": true`; active links by `data-match`, `data-match-exact`, and
      `nav-key`; `@auth` variants; ETags after expansion; `partial_put`, `partial_get`,
      `partial_list`, `partial_delete`, and `route_check`'s `partials`.

### Phase 16b: trying content without writing it (007)
- [x] `page_render` previews unsaved pages, templates, partials, and files, and a listing's
      feed; `template_get` reads any template, built-ins too; a page's `json_ld` field; Atom
      author and categories; `content_put` sha256; csv, atom, webmanifest, and other types.

### Phase 16c: deploys that can't leave the site down
- [x] `/goober/ready`: build, uptime, and each subsystem (storage, identity, boxes, mcp,
      queue, change log, traffic), 503 unless all are fine; `host_health` lists each box's version.
- [x] The panel's stores never stop the host from starting; it serves without them and says so.
- [x] The server keeps the previous build; a new one not ready within a minute is rolled
      back, with its log printed. `tools/readiness.py` then checks from outside (pages,
      discovery, MCP, boxes), with per-site lists in `~/.config/goober/readiness.d/` on the deploying machine
      (`tools/readiness.d/` holds a template).
- [x] The server runs readiness.py on itself after each deploy and publishes it at
      `/goober/deploy` (and `deploy` in `/goober/ready`), for bots' post-deploy checks.
- [x] Caddy holds requests through a restart (`lb_try_duration 15s`) and answers a retryable
      JSON 503 with Retry-After when the host is down; the deploy adds both to an existing
      goober Caddyfile after `caddy validate`.
- [ ] The root cause of the 2026-10-29 outage, from the server's log (the human's step).

### Phase 17: the operator panel (028)
- [x] The panel on `operations.domain` (never a tenant's) or `operations.listen`, signed in as a
      host identity with `host:owner`, `host:operator`, or `host:observer`; look-only.
- [x] Traffic rollups derived from the access and egress logs (`operations` package): a
      tailer with a cursor, rotation-safe, backfilled from the logs on first start.
- [x] The change log from the host WAL's observer, and operator actions linked to their changes.
- [x] Tools `traffic`, `hot_boxes`, `box_graph`, `changes`, `change_get`, `operator_actions`; the
      read-tool set, which is all `host:observer` may call over MCP.
- [x] Views: overview, traffic, data flow, boxes, one box, change log with diffs, database.
- [ ] On example.com: an `A` record and Caddy line for `ops.example.com`, and
      `operations.domain` in host.json (the human's step).
- [ ] Approvals (305), tuning, counters, traces, and cost.

### Phase 18: progressive web apps (782)
- [x] Apps over a scope (the whole domain or a part), the generated manifest, worker
      (offline, push), and register.js under /goober/pwa/$app/, and `<!--#include pwa-->` in
      the built-in base template.
- [x] Web Push: VAPID and aes128gcm (`webpush`, on static OpenSSL 3.5.1), subscriptions with
      topics, a box `push` service through the outbox, gone subscriptions dropped.
- [x] Tools `pwa_put`, `pwa_get`, `pwa_list`, `pwa_delete`, `pwa_subscriptions`, `push_send`.
- [x] Checked in headless Chromium: the worker controls the scope, no installability errors,
      offline pages and the fallback.
- [ ] A real push end to end on a phone (the human's step), and a panel view of apps.

### Phase 19: a community that meets by mail (783)
- [x] `"markdown"` on `send` (M6); `"attachments"` on `send` (M4); `"invite"` on `send`:
      RFC 5545/5546 invites (the `calendar` package); RSVPs read back as `calendar`.
- [x] `reply_text` on received mail: the person's own words, without the quoted digest.
- [x] The recipe in `box_guide` and 783.
- [ ] Bounces and complaints reaching the box (M2); the community box itself (Grok builds it).

### Phase 20: host libraries (831)
- [x] `"libraries"` in manifests, checked at upload; `goober_lib` in the ABI and the Rust binding;
      `library_list` and `library_get`; calls and tzdata on the access line.
- [x] `time@1`: now, parse, format, to_utc, add, diff, start_of, zone, zones, recur.
- [x] `ics@1`: parse, build, invite, respond (REPLY or COUNTER), interpret, counter_accept,
      counter_decline, expand; the `calendar` package (RFC 5545 model, RRULE, VTIMEZONE, zones).
- [x] Mail speaks ics@1: invites, `"ics"` on send, `calendar` on received mail.
- [ ] Writing VTIMEZONEs; VTODO helpers; free/busy; a `locale@1`.

### Phase 21: operator scope (030)
- [x] Each operator (a person, a token, the config's token) acts on all tenants or a set, kept in
      `storage/operations/scopes.json`; every operator tool has a scope rule, deny by default.
- [x] Changed only in the panel's Operators view (an owner sets anyone's; an operator narrows
      their own); `operator_scope` tells an agent where it stands; `token create --tenants`.
- [x] The panel narrows itself for a focused person.

### Phase 22: invites, consent, and mail preferences (029)
- [x] Invites in the realm: the record, the host's `invite` template with links no template can
      drop, accepting by code (roles, terms, lists), declining as a block, one reminder, lapse
      and cooling; `invite_*` tools; a box's `invites` service with roles the config allows.
- [x] Mail categories (reply, transactional, confirm, outreach, list) checked at the call and
      at delivery; the host's footer with the postal address; `List-Id`.
- [x] Lists with double opt-in (`subscribe`, `/goober/list/`), the preferences page, the
      account page's Mail section, and `consent.db` with a five-year log.
- [x] Blocks lifted only by the person (signed in, with a statement) or in the panel.
- [x] `tenants` in the host config: postal address, terms, privacy, invite roles and limits.
- [ ] SES bounces and complaints as blocks (026 § 6); a host-wide block; data subject requests.
      (CASL's one-message referral rule isn't planned: the human chose US law, 2026-10-30.)

### Phase 23: the content kit (031)
- [x] /sitemap.xml and /llms.txt from pages' front matter, a sitemap index for boxes' sitemaps.
- [x] Posts: a built-in post template, BlogPosting JSON-LD, canonical, OG/Twitter, rel
      prev/next, list-template, authors in feeds.
- [x] content_deploy (sha and etag before, signed-out checks after), list_etags, url_check,
      page_lint; etags on page_get and if_etag on page_put.
- [x] Compact page_render, build logs, and diffs (verbose for the rest); aliases,
      ignored_arguments, schema hashes, tool_schemas; the tenant filled in when only one fits.
- [x] Box pages in the site's layout (front matter `layout: base` in a box template, now
      documented and tested); box_render previews; version_upload dry_run; noindex in base.
- [ ] Pinned partials and parameters (031 § 7); an approvals ledger (031 § 7): waiting on the human.

### Phase 24: logs, performance, and auditing agents (032)
- [x] The `logs` package: an envelope (at, log, seq, host), a hash chain through every line and
      across rotations and processes, daily and size rotation with manifests, verify, search.
- [x] Every host log through it, addresses as keyed hashes; an `audit` log of operator tool
      calls, auth failures, sign-ins, panel writes, reloads, start and stop, and CLI tokens.
- [x] Rotated logs uploaded sealed with their manifests, kept 400 days; 7 days on the machine.
- [x] Live per-URI statistics and flow in memory; `uri_stats`, `uri_tree`, `uri_flow`,
      `agent_audit`, `log_search`, `log_verify`, `log_status`.
- [x] Panel views: Sitemap (an icicle of the site), Performance, Agents, Content, Logs.
- [ ] Alerting a person when a flag goes up; S3 object lock (set outside goober).

### Phase 25: speaking HTTP to any API (033, 034)
- [x] The research (033): description formats, an audit of popular APIs, the gaps.
- [x] `vault` (sealed values bound to tenant, service, and owner), `specs` (pinned OpenAPI
      2.0 to 3.2 with operations by id or `VERB /path`), `signing` (OAuth 1.0a, checked against
      X's published example).
- [x] Manifests: `spec`, `op`, auth kinds (header, bearer, basic, query, oauth1), `credentials:
      "sealed"`, `connect` (paste, oauth2 with PKCE), constant headers, json/form/multipart/raw
      bodies, chunked uploads, polling, `error-when`, `limits`, `any-public`.
- [x] Host-run connections at /goober/connect/ with single-flight refresh and remembered
      generations; spec_put/list/get; specs in source projects; `goober-host vault key`.
- [ ] Pagination, resumable and presigned-POST uploads, GraphQL methods, JWT and template HMAC
      signers, Overlays, Bluesky OAuth, inbound webhook verification; OAuth 1.0a's three-legged flow.

### Phase 26: approvals per connected agent (035)
- [x] A switch per agent (OAuth agent, token, or the config's token), off by default, flipped
      in the panel's Operators view by an owner (or an operator for their own agents).
- [x] Changing calls held as requests; read tools and `approval_status` never wait; approve
      runs the call as the agent, deny never runs it; requests expire in a day; audited.
- [ ] Standing rules per tool or tenant, 305's version registry and modes, notifications.

### Phase 27: the bus (040, 036)
- [x] `bus` package; open HTTP streams; `publish` and `routes.sse` in manifests.
- [x] Publish on commit; read-only, coalesced, shared evaluations; Last-Event-ID; limits;
      keepalive; `bus_status`; tests end to end.
- [ ] NATS for a cluster; a JavaScript helper in the asset kit.

### Phase 28: the deploy as an operations audit (028 § 8)
- [x] `goober-host audit`: the panel, Caddy's sites, the mailer, backups, operators;
      `audit.quiet` in host.json; tests.
- [x] Every deploy runs it and offers to set up the panel at `ops.` once its DNS is ready
      (or never to ask again), checked and put back if refused.
- [x] Missing settings asked for at deploy: the postal address per tenant, the SES from
      address, the backup bucket, the AWS keys, an operator; applied, checked, put back.
- [x] Redirect domains (`{"redirect-to": ...}` in `domains`, a 301 or 308 from the host); the
      audit suggests `www.` for a site, and the deploy offers it (yes, not now, never) and adds
      it to Caddy's site line, checked and put back if refused; tests.
- [x] `just deploy ... --add-tenant [DOMAIN]`: a wizard for one new site (domain, tenant, owner,
      postal address, terms and privacy, www.), shown the DNS it needs, added to host.json and
      to Caddy as a site block of its own, checked, proved by a restart, put back if refused;
      flags for the answers without a terminal; `tools/tenant_edit.py`, tested in CTest.
- [x] Stalwart is provisioned only when something it is made from changed or it isn't up (a
      fingerprint kept beside its scripts); `--reprovision-mail` forces it (024 § 7).
- [x] Backups never enter subtrees they don't hold (tmp/, build caches), and name the path,
      owner, and mode of anything they must read and can't; every deploy gives storage back
      to the host's user (`find -execdir chown -h`); the audit's `storage` check; tests.
- [ ] More checks as operating teaches them: certificates' expiry, disk, the backup key and
      vault key copied off the machine.

### Phase 29: reads, caching, and performance reports (037)
- [x] GETs and HEADs change nothing: writes and effectful calls refused, reads begin
      deferred beside a writer; the leads box's mailed link confirms by a button; tests.
- [ ] Per-call time split and per-statement statistics; `perf_report`, `perf_statements`.
- [ ] Database versions; the route `cache` field with checks; 304s without the box;
      `cache_status`, `cache_clear` (waiting on 037 § 9's decisions).

### Phase 30: source in the panel, and its history in the bucket (020 § 9 to § 12)
- [x] Builds and deploys kept with their logs; deploys and the box versions each uploaded;
      `source_tree`, `source_box_history`, `source_job` by project; `version_list` names a
      version's source revision.
- [x] The panel's Source view: projects, revisions, colored diffs, files with line numbers,
      build logs, and a box's versions back to their source.
- [x] The mirror: every revision as a sealed `git format-patch` patch (binary files as git
      binary patches), every deployed revision as a sealed snapshot with its deploys and
      logs, under `source/` in the backup bucket, never rotated; `goober-host source-mirror`
      reads it back; `git am` replays it (tested).
- [ ] Untried against real S3, like the rest of the backups.

### Phase 31: grants as a box's limits, down to the table (008 § 4.1)
- [x] Every way into an ungranted table or column refused (subqueries, joins, CTEs,
      `RETURNING`, upserts, `sqlite_master`, `pragma_*`, views, triggers, temp tables,
      `ATTACH`), probed in one test.
- [x] Use recorded from the authorizer per box version; `grant_usage` compares grants with
      use (unused tables, writes, columns; `true` where a list would do, with the list).
- [x] The panel's graphs show each database's tables with the operations granted, dashed
      while unused; a box's Grants card.
- [x] Upload advice for `update: true` on wide tables; `box_guide` says to grant only what a
      box uses.
- [ ] Use is kept in memory since start; a long window (kept across restarts) would make
      `unused` mean more for routes that run rarely.

### Phase 32: host ids, and jobs that move data (038)
- [x] The id service: a counter in `storage/ids/ids.db`, ranges of 10,000 vended durably,
      a first start past every stored id (and from 2^32 + 1), `ids_status`, `goober-host ids
      bump`, and rehydrate's jump; tests.
- [x] Users, invites, and uploads take their ids from it.
- [x] The `id` column type: keys stamped by the host (`DEFAULT (goober_id())`, triggers that
      refuse a key a box names or changes), `ids` in `goober_sql`'s answer, ids as strings in
      JSON, warnings for keys and references that aren't ids, `box_guide`; tests.
- [x] Jobs (`routes.job`): operator-started, dry runs on copies required first, batches with
      a kept cursor that resume after a restart, pause and go on, `job_*` tools, the panel's
      Jobs card, `box_guide`; tests.
- [ ] The example boxes' schemas moved to `id` keys (new schema versions).
- [ ] Operator tools take and give ids as strings (attachments, invites, users), as 021 says.
- [ ] Identity events, the outbox, approvals, and other host tables still number their own
      rows (038 § 11).

### Mail, seen from the tools (a site's engineers' asks; 018 § 7, 024 § 7)
- [x] `mail_template_send_test`'s mail shows in `mail_activity` (`test: true`); dev mail's log
      lines carry the tenant and status like SES's and SMTP's.
- [x] `mail_domain_check`: MX (this host's? port 25 tried on its own address only), SPF, DMARC,
      and SES's identity, verification, and DKIM tokens with the records to add; tests on DNS
      answers parsed from wire bytes, and SES against the stand-in.
- [x] `host_health.smtp_listener`: the banner on 127.0.0.1:25, or why not.
- [x] `mail_route_check` (the hook's own matching) and mail addresses in `box_list`.
- [x] A per-sender daily limit as a `box_guide` recipe (the human chose boxes over a manifest key).
- [x] Postal-address refusals and `host_health` say a person sets it at deploy or in host.json;
      no tool sets it (the human's choice).
- [ ] `mail_domain_check` untried against real DNS and SES.

### Phase 33: deleting an account (021 § 6)
- [x] The person deletes their account on `/identity/account` (typed `DELETE`), or an operator
      with `user_delete` (audited, tenant-scoped); sessions, grants, roles, and codes end; the
      address leaves the realm; the id stays a tombstone; the same address may sign up anew.
- [x] `routes.identity` `deleted`, called as nobody without the address; each box's answer is
      its acknowledgement; `deletions` and the panel show who hasn't, and what is overdue past
      30 days; `box_guide`; tests.

### The engineers' friction, 2026-10-30: source and box lifecycle
- [x] Staged commits: `source_stage` builds one commit over several calls, kept in source.db
      for an hour; `source_commit {stage}` commits it whole (020 § 13); tests.
- [x] `schema_check`: schema_put's checks and warnings, nothing stored; a build's "isn't
      stored" error points at goober.project.json's `schemas`; tests.
- [x] `version_upload {activate: false}` stores a version dark for version_promote; tests.
- [x] `box_disable {box, why}` / `box_enable`: a box out of service and back, kept across
      restarts, in version_list and the change log; tests. One route alone: deploy a version
      without it.
- [x] A call that may just return (a timer, an identity event, mail) no longer logs
      `no_response` in the box's log; test.
- [x] schema_put, the key refusal, and `box_guide` say a natural key is a unique index, not the
      primary key.

### Content: etags, templates, Vary, change ids (a site's engineers, 2026-10-30; 031 § 3, 007 § 8)
- [x] Every tool says which etag: `file_etag` (stored, for `if_etag`) or `body_etag` (served);
      `etag` kept as it was.
- [x] Templates: `file_etag` in `template_get` and `template_list`, `if_etag` on `template_put`
      and `template_delete`, templates and partials in `list_etags`; tests.
- [x] Templated box routes send `Vary: Accept` on every answer, merged with the box's; test.
- [x] Write tools answer the `change_ids` they made; test.

### The engineers' questions, 2026-10-30 (late): moving routes, dry runs, the guide's answers
- [x] A route or mail address moves between boxes: a dark version that clashes with a live box
      is stored with a warning; box_disable the old, version_promote the new; box_enable on the
      old is refused while the new holds the names, saying how to roll back (005); test.
- [x] `version_upload {dry_run, schemas}` checks a manifest against schemas not stored yet; test.
- [x] `box_guide`: how path and mail-address parameters are decoded (tested), the exact list of
      SQL functions (json_each and json_tree included; tested), ids from before the service and
      what an empty `ids_status` means (now with a `note`), calls that may return without
      answering, and that `box_render` runs neither the box nor the gate.
- [x] Box SQL has no clock (the human's call, 2026-10-31): 'now', CURRENT_TIMESTAMP and its
      kin, and date functions with no value are refused, and the statement is undone (a VFS
      that interrupts a box statement asking for the time); operators keep the clock; tests.

### Box flags (039)
- [x] `"flags"` in the manifest (a closed set with defaults and optional values), the
      `goober_flag` import (and `goober::flag` in Rust), values per tenant and box kept through
      the host WAL; `flag_set` (audited, change log, approvals) and `flag_list`; what each call
      read in the access log; the panel's Flags card; `box_guide`; tests.

### Databases for migrations, and drops only a person makes (008 § 11; 011)
- [x] `db_create` (stored schemas, before any box binds it), `db_snapshot`/`db_snapshots`
      (online backup, SHA-256, row counts; 100 files and 2 GiB a tenant), `db_restore_to_new`
      (never over an existing database), `db_unbound`; tests.
- [x] Drops in the panel only, by host:owner, the name typed: a database, a table, a
      snapshot, or retiring a schema; refused while any box (active or disabled) binds it;
      snapshot first; audit log and change log; retired schemas refused at upload; tests.
- [x] A column's `"not-a-reference": true` silences the reference warning; tests.
- [x] Types: `const` accepted; `pattern` refused until the host has a linear-time regex
      engine, with that reason in the error and `box_guide`; tests.
- [ ] A linear-time regex engine, so `pattern` can be accepted.

### Later, not planned yet
Domain registry (015), configs and rewrites (012), write sets (025),
access patterns and generators (020, 028, 110, 120), sourcing (023),
mail routes (038), the upload verb (060), the key vault (064), approvals, deployments,
bundles, working sets (300 to 320), stability (330), quotas (390), the cluster (400),
and contraction (420). Each waits for the human to pull it forward.

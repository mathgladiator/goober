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
| 015 Domains | none | Domains live in the host config (005 §6). | The registry table, primaries and redirects, verification, TLS ask hook. |
| 018 Reserved | partial | `{!tenant}`, `{!user_id}` in requirements; `{!email}`, `{!user_id}`, `{!tenant}`, `{!domain}` in `require`; `client_id` for agents and a `caller` block for internal calls in the request document; `!` names refused. | Substituting `!client_id` and the rest in requirements; one-setter enforcement across protocols. |
| 019 Conflicts | partial | Overlapping routes refused at upload; across boxes a literal beats a parameter; `route_check` names what answers a path and what it shadows. | Traffic decisions, given-up routes, static and rewrite conflicts. |
| 020 DataFlow | partial | `data.sqlite3.$strategy` with `pattern: "sql"`, `{!tenant}` database names, a fixed `tenant` per strategy so one database spans domains, and boxes sharing databases with their own grants, their schemas checked to fit (008 §4). | `by_id`, `by_index`, named writes, generated row structs, data signatures. |
| 021 PrimaryKeys | none | SQLite rowids (008 §9). | Host-minted ids, range leases. |
| 023 Sourcing | none | | Sources, `append`, event routes, cursors. |
| 025 DataShape | none | A SQLite transaction per database per request (008 §6). | Write sets, bundles, layered reads, leases, worst-case checks. |
| 028 Query | partial | Direct SQL under SQLite's authorizer, row limits, step budgets. | `pattern: "query"` iterators, plan checks, pagination cursors. |
| 030 Larger env. | partial | `external` services by verb and path, `require` pins from the context, credentials by key name, per-request budgets, never past the request's deadline, an egress log (015); box-to-box calls (016); the web as a service through the host's robot (017). | OpenAPI documents and generated clients, async calls, per-service rate limits. |
| 035 Outbox | partial | Elements committed with the request's writes, a dispatcher delivering each service from its own oldest element (one stuck service never blocks another), backoff, stable idempotency keys, dead letters and retry, and stranded outboxes found by scanning storage (015). | Result routes, delivery to internal routes. |
| 036 Json | partial | The types document in the strict subset (plus `date-time` and `date`), stored with the version, used to check bodies, queries, tool inputs, and typed responses; int64 as strings; defaults filled (011). | `token`, change classification, generated structures and serializers. |
| 037 Http | partial | Typed path params, verbs, cross-site POST refusal, ETags on content and box GETs; declared `body`, `max-body-bytes`, `query`, `request-headers`, and typed `response`; the status vocabulary; CORS per route (011). | ETags that skip the box, compression, ranges, declared redirect targets; undeclared routes still see every header and parameter. |
| 038 Mail | partial | Mail in through Stalwart and `routes.mail`, with reply tokens and the box's answer as the SMTP answer (024). Mail out through SMTP or Amazon SES; boxes send through a host `mail` service, via the outbox, within per-tenant and per-address daily limits; a sandbox sends nothing (018). | Verified senders (Q39), verified senders per domain for mail out, `send-email@1`, bounce handling. |
| 039 Internal | partial | Internal routes with callers, synchronous calls carrying the person and tenant, both sides checked, cycles refused, depth capped, deadlines shrink (016). | Outbox to boxes, idempotent routes, typed generated calls, tenant binding by path, worst-case sums. |
| 040 Bus | none | | Signals, `publish`, SSE subscriptions. |
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

### Phase 15: the email worker and kcmeatup (026)
- [x] Outreach mail: a service's own `from`, threading headers, one-click unsubscribe and a
      per-tenant list (018 § 3).
- [x] Result routes and two-minute deliveries, for model calls (015 § 3).
- [x] Replies in a thread the person started get their own daily allowance.
- [x] Host-built JSON-LD for box templates (007 § 8).
- [ ] M1: Stalwart's DMARC strict, once real mail arrives (Q39).
- [ ] M2: SES bounces and complaints through SNS, unsubscribing hard bounces and complaints.
- [x] M3: `mail_activity`, a tenant's mail in and out for operators and agents.
- [ ] M4: attachments on `send`.
- [x] M5: per-tenant daily caps on outbox calls per service (`limits.outbox-daily-per-service`).
- [ ] M6: markdown in `send`.

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
      discovery, MCP, boxes), with per-site lists in `tools/readiness.d/`.
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
- [ ] On kcmeatup.com: an `A` record and Caddy line for `ops.kcmeatup.com`, and
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

### Later, not planned yet
Domain registry (015), configs and rewrites (012), write sets and host ids (025, 021),
access patterns and generators (020, 028, 110, 120), sourcing (023), the bus (040),
mail routes (038), the upload verb (060), the key vault (064), approvals, deployments,
bundles, working sets (300 to 320), stability (330), quotas (390), the cluster (400),
and contraction (420). Each waits for the human to pull it forward.

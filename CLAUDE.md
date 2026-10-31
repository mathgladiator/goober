# CLAUDE.md

## Phase: design, with a first host to play with

Goober is still mostly in the **design and context-building phase**. A first host (v1)
lives in `host-src/`, built at the human's request so they can play with the design. Its
decisions are recorded in `design/machine/005.FirstVersion.md`. On 2026-10-26 the human
asked Claude to keep executing the whole design, toward software they run for themselves
and can build products on. `PROGRESS.md` at the repo root is the plan and the honest
account: every human doc's status, what is faked or missing, and a checklist Claude works
through. Keep it true with every change, and grow the code along that plan, not beyond it.
`READINESS.md` answers, briefly, how ready goober is for the human's experiments (static
hosting, Grok deploying boxes, agent outreach, the meat index); keep it true too.
`HUMAN.md` is the human's one-page check while Grok runs the host (is it up, what Grok did,
errors, backups and mail, redeploy, stop Grok, undo); keep every command in it working.

## The host (`host-src/`)

- C++20, CMake with Ninja presets, GoogleTest through CTest; dependencies under `~/.local`
  (Boost, GoogleTest, wasmtime v49.0.4 and a static OpenSSL 3.5.1 in `~/.local/opt/`, from
  `host-src/tools/install-deps.sh`).
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
  - `design/human/521.Source.md`: source kept in the host: projects with numbered revisions an agent checks out, commits to (conflicts only on the same files), builds in the host's sandbox, and deploys over MCP, so Grok's code lives in the machine. Drafted by Claude at the human's request.
  - `design/human/663.Backups.md`: backups off the machine to S3: attachments queued and uploaded once, a nightly sealed tar.gz kept fourteen days by its timestamp, database increments between nightlies, and rehydrating any moment onto a sandboxed devbox. Drafted by Claude at the human's request.
  - `design/human/800.Crawl.md`: goober as a good robot: polite fetching through the host (robots.txt, pacing, public addresses only), views that simplify pages (text, markdown, links, meta, JSON-LD, tables, select, extract), crawls, and the local meat index as the example. Drafted by Claude at the human's request.
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
    the operator's db tools and CLI, and grants as a box's whole reach with `grant_usage`
    (§ 4.1).
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
  - `design/machine/015.Network.md`: `external` services through `goober_call`, the outbox
    committed with the request, delivery with retries and idempotency keys, and the key store.
  - `design/machine/016.Internal.md`: internal routes with callers, synchronous box-to-box calls
    as the same person, both sides checked at upload, no cycles.
  - `design/machine/017.Crawl.md`: the robot (robots.txt, pacing, address checks, cache,
    opt-out), page views, operator crawls and tools, and the `web` service boxes call.
  - `design/machine/018.Mail.md`: Amazon SES as the host's mailer, AWS keys in the key store,
    a `mail` service boxes send through via the outbox with daily limits, and the sandbox flag.
  - `design/machine/019.Backups.md`: S3 backups: sealing, the bucket's layout, the nightly,
    page-level increments, assets, rotation, `goober-host rehydrate`, and the tests.
  - `design/machine/020.Source.md`: source projects over MCP: storage, concurrency,
    `goober.project.json`, sandboxed Rust builds with vendored crates, deploys, and the tools;
    kept jobs and deploys (box versions back to revisions), the sealed mirror in the backup
    bucket (git-am patches and deployed snapshots, `goober-host source-mirror`), and the panel's
    Source view.
  - `design/machine/021.IdentityEvents.md`: accounts exist once an address is proved (invites
    expire in 30 days), sign-in events, and `routes.identity` for first verifications and sign-ins;
    deleting an account (§ 6): erased with a tombstone id, a `deleted` event boxes acknowledge
    within 30 days, `user_delete`, and `deletions`.
  - `design/machine/022.MailTemplates.md`: every mail rendered from a template (subject, text,
    HTML, a layout), defaults for each host mail, overrides per tenant or host, and box templates.
  - `design/machine/023.OperatorOAuth.md`: a second OAuth issuer, `/oauth/operator`, in the host
    realm, so agents that only speak OAuth (Grok) sign an operator in and reach the operator tools.
  - `design/machine/024.MailIn.md`: mail in: Stalwart on port 25 calling the host's MTA hook,
    `routes.mail` addresses, the box's answer as the SMTP answer, reply tokens, and deploys.
  - `design/machine/025.Telephony.md`: a proposal: a phone number per tenant from a rented
    provider, `routes.phone` for texts and calls, a `phone` service for texts out, host-config
    grants checked at upload, spending as `host:owner`, STOP/HELP in the host; Q42 to Q49 open.
  - `design/machine/026.CaseStudy.md`: a case study: a directory run by an agent team, what it
    needed, how the agents behaved, the email worker recipe, and needs per goal.
  - `design/machine/027.Partials.md`: shared partials: `_partials/$name.html` included by
    `<!--#include name-->` in pages and box HTML, active links by `data-match`, `@auth` variants,
    ETags after expansion, and the `partial_*` tools.
  - `design/machine/028.Panel.md`: the operator panel: its own origin (`operations.domain` or
    `operations.listen`), host-role sign-in, look-only; traffic rollups from the access log, the
    change log from the WAL's observer, data-flow graphs, the database explorer, `host:observer`;
    the operations audit every deploy runs (`goober-host audit`, `audit.quiet`), which offers the panel
    and `www.` redirects (yes, not now, never; Caddy too) and asks for missing settings (postal
    address, mailer, backups, AWS keys, operators); `--add-tenant` launches one more site.
  - `design/machine/029.Invites.md`: invites as a host feature (the record, the template, accept by
    code with roles, terms, and lists; decline as a block only a person lifts), mail categories with
    their rules, lists with double opt-in, the preferences page, and per-tenant postal addresses.
  - `design/machine/030.OperatorScope.md`: focusing an operator (a person, a token, and every agent
    signed in as them) on some tenants: a scope rule per tool, deny by default, changed only by a
    person in the panel.
  - `design/machine/031.ContentKit.md`: a site's engineers' requests: platform sitemap.xml and
    llms.txt, markdown posts with a built-in post template and BlogPosting, content_deploy with
    checks before and after, url_check, page_lint, compact output, and stable tool schemas.
  - `design/machine/032.Logs.md`: logs with an envelope and a hash chain, daily rotation, sealed
    archives kept 400 days, addresses as keyed hashes, an audit log, the log dive, live per-URI
    p99 and flow in memory, agent auditing, the panel's new views, and a SOC2 mapping.
  - `design/machine/033.HttpFormats.md`: research: the ways HTTP APIs are described (OpenAPI 2.0
    to 3.2, Overlays, Arazzo, Discovery, Smithy, and more), an audit of popular APIs (Amazon
    aside), and the patterns that break "just OpenAPI".
  - `design/machine/034.SpeakingHttp.md`: a small language for any API: pinned OpenAPI specs,
    auth appliers, sealed per-user secrets, host-run OAuth 2 and paste connections, form,
    multipart, and raw bodies, chunked uploads, polling, limits; Hevy, Strava, X, and POSSE.
  - `design/machine/035.Approvals.md`: approvals per connected agent: a switch in the panel, off
    by default; when on, the agent's changing tool calls wait as requests a person approves (run
    as the agent) or denies; read tools never wait.
  - `design/machine/036.Bus.md`: the bus in-process: `publish` on strategies moved on commit,
    `routes.sse` subscriptions evaluated read-only, coalesced, and shared, over SSE.
  - `design/machine/037.Caching.md`: reads and caching: GETs change nothing (built); per-database
    versions, a route `cache` field (by data or by wake, fresh-ms, browser-ms, sampled checks),
    304s without the box, and MCP performance reports; decisions open in § 9.
  - `design/machine/038.Ids.md`: the engineers' report that ids didn't follow 021, and the fix: one id
    service (ranges, a first id of 2^32, restores jump past), users, invites, uploads, and `id` keys
    the host stamps (a box never names one), ids as strings, schema warnings; and `routes.job`,
    operator-started one-time boxes that move data, dry run first, in batches with a kept cursor.
  - `design/machine/039.Flags.md`: box flags: a closed set the manifest declares (`"flags"`, defaults,
    optional values), read through `goober_flag`, set per tenant by operators with `flag_set`
    (audited, in the change log), so a freeze or kill switch needs no new version.
  - `design/machine/782.PWA.md`: progressive web apps: the audit of what PWAs need, apps over a
    scope (`_apps/$name.json`), the generated manifest, worker, and `<!--#include pwa-->` tags,
    offline caching, and Web Push (VAPID, aes128gcm on OpenSSL) through a box `push` service.
  - `design/machine/783.ThesisCommunity.md`: a community that meets by mail (jeffrey.io's agentic
    social network): the audit, markdown on `send`, `reply_text`, calendar invites and RSVPs,
    attachments on `send`, and the box recipe (one digest a day, one reply a day).
  - `design/machine/831.Libraries.md`: host libraries: closed, pure, versioned function sets a
    manifest imports (`"libraries": {"time": "time@1"}`) and calls through `goober_lib`; time@1
    (zones, calendar arithmetic, recurrence) and ics@1 (iCalendar, invites, replies, proposals).
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

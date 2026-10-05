# CLAUDE.md

## Phase: design

Goober is in the **design and context-building phase**. There is no implementation yet.
Don't write code, scaffolding, or build files unless explicitly asked. The human is
working through the implementation choices personally.

## Design documents

Read these before doing design work:

- `design/human/`: written and maintained by the human. **Authoritative and
  protected.** Treat it as the source of intent. Your edits here are limited to
  grammar, syntax, and clarity, so the text makes sense while saying exactly what the
  human meant. Never add, remove, or change ideas, positions, or decisions. If a
  clarity fix would shift the meaning, or if you think the content itself is wrong,
  propose it in conversation or in `design/machine/` instead.
  - `design/human/000.Introduction.md`: the introduction and foundational thinking
    (the bet, boxes, manifests, storage, process model, the five arenas).
  - `design/human/005.Goals-And-NonGoals.md`: what the experiment is for and against, and
    how we will know it worked.
  - `design/human/010.Manifest.md`: the manifest shape, the generated host/guest C ABI
    (WASM reactor pattern), behavior (boundary, lifecycle, budget), and sharing rules.
  - `design/human/015.Identity.md`: OAuth sessions, grant roles (flat strings, `/` namespaces),
    deny-by-default requirements, grantable/revocable, the login API, anti-phishing UI.
  - `design/human/020.DataFlow.md` (draft): context sources, `data.$provider.$strategy`,
    access patterns, generated data code, per-tenant databases, and SQL forcing solo.
  - `design/human/025.DataShape.md`: storage shapes and invariants, sequencers and cache
    coherency, atomic write sets (optional per route), and torn-write reporting.
  - `design/human/030.TheLargerEnvironment.md`: networking through the host, external
    services via OpenAPI, `require` restrictions, host-held credentials, box-to-box calls.
  - `design/human/035.TheOutBox.md`: effectful network calls as outbox elements in the
    write set, delivered after commit with idempotency keys and per-scope ordering.
  - `design/human/036.Json.md`: JSON types borrowed from OpenAPI `components.schemas`, a
    strict subset mapped to C, and safe vs. breaking type changes.
  - `design/human/037.Http.md`: declared query, headers, and bodies; the status vocabulary;
    validation, CORS, and ETags handled by the host.
  - `design/human/038.Mail.md`: mail routes via Stalwart MTA hooks, verified senders as
    principals, signed reply-address capabilities, and outbound mail via the outbox.
  - `design/human/040.TheBus.md`: pub/sub as monotonic signals (not data), coalesced
    SSE subscriptions that re-evaluate a route, publish-on-commit, and bus options.
  - `design/human/045.Time.md`: timer routes and data-driven schedules (plain `-field` refs).
  - `design/human/050.AgentsMCP.md`: MCP tools/resources mirroring HTTP; agents limited to
    mcp routes; isolate agents in their own box.
  - `design/human/060.Files.md`: upload verb, preprocessing, ledger states, asset routes, GC.
  - `design/human/064.Keys.md`: vault-held root key, envelope encryption, `keys/<scope>/...`.
  - `design/human/070.UI.md`: content negotiation, markdown, Mustache, static, objects.
  - `design/human/099.TheSystem.md`: the operational surroundings; rent the edge to keep the
    host small.
- `design/machine/`: drafted by Claude. Proposals until a human ratifies them.
  - `design/machine/PHILOSOPHY.md`: engineering philosophy carried over from an earlier
    project. It references C++, CMake, Boost, and `goo::system::System`, none of which
    has been decided for goober. Treat it as a candidate, not a rule.
  - `design/machine/feedback-2026-10-04.md`: a long-form review of every human doc, with
    limits, proposals (identity/MCP, timers, fetch, compatibility views, adapters),
    a security evaluation, and the Hearth and blue-collar OS goals.

If a machine doc conflicts with a human doc, the human doc wins. Flag the conflict
rather than resolving it silently.

**When asked to design something, write the design document in `design/machine/`.**
Name files `NNN.<Topic>.md`, where `NNN` is a three-digit numeric prefix chosen so
that lexical order is the ideal reading order (for example, `000.Introduction.md`).

`README.md` summarizes the human intro. Keep it in sync when the human docs change.

## Precision and decisions

**Manifest naming rule.** Platform-defined manifest keys are lowercase words joined by
hyphens (`timeout-ms`, `at-field`, `id-is`). Names people choose (route names, strategy
labels, context variables, type fields) use underscores, since they become C identifiers.
Keys from a borrowed format (OpenAPI, JSON Schema) keep that format's spelling. Apply this
to every new example and flag violations when editing.

This project demands extreme precision. Be exact in wording, names, paths, and
invariants. Don't paper over ambiguity.

**Stop when a decision feels major.** Examples include choosing a language, runtime,
or library; wire or storage formats; the manifest schema; trust and authority
boundaries; naming conventions; and anything that is costly to reverse. Lay out the
options and tradeoffs, recommend one, and wait for the human. Don't pick quietly and
keep going.

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

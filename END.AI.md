# Making the next version much better

Claude's thoughts at goober's end, written 2026-11-07 at the human's request. They are
proposals for the next project's human design documents, not decisions.

## 1. What went wrong, including my part

- **Every request became a feature.** Each ask turned into a design document, code, tests, tools,
  and a panel view, built the same day. By the end there were about 190 operator MCP tools,
  61 machine documents (about 183,000 words), and a PROGRESS file of 61 phases. Each piece was
  defensible. Together they were more than one person could hold.
- **Decisions piled up while the building went on.** 049, 050, 052, 054, and 055 each ended with
  "decisions for the human", close to twenty open at the end. I wrote them down and kept going on
  my own recommendations. That is how a system ends up steering its owner.
- **Agents didn't know what already existed.** The last friction report asked mostly for features
  that already existed (roles a box grants, lazy schemas, client addresses, fragments kept through
  sign-in). With a surface that large, adding one more feature makes the next agent less likely
  to find the right one.
- **We built a harness inside the host.** Source projects, sandboxed builds, skills as memory,
  `route_try`, staging, change sets, and per-call approvals all rebuild, through MCP, what a
  repository, a test runner, git, and a diff review give for free. They exist because of the
  medium (an agent in a chat window) more than because of the product.

## 2. Choose the medium first

The next project should assume agents build in a real engineering harness: a repository, local
builds and tests, and git as memory and history. The host then has a smaller job:

- **Run versions:** accept a built artifact and its manifests, check them, and serve them.
- **Keep the data:** databases, files, and backups.
- **Show what happens:** logs, traffic, errors, and the operations views.

MCP stays for operating (looking, diagnosing, flipping a flag in an incident), not for writing code.
A small fixed set of tools, under about 30, each one obvious from its name.

## 3. Split the manifest

One manifest per box grew to carry routes, types, data, grants, services, limits, and more. Split
it along the isolation line:

- **A data manifest** owns tables, their shapes, and their invariants. It is the one place data
  rules live, and it changes rarely and carefully.
- **A routing manifest** owns HTTP, mail, timers, and tools: who may call what, and which data
  operations each route may perform.
- **Between them, pre- and postconditions** act as the barrier. A route states what must be true
  before it runs and what it promises after. The host checks both, so a bad route shows up as a
  refused write rather than corrupted data.

Give each kind a size budget. A manifest that outgrows its budget is a sign the box should split.

## 4. Beta as the default, production by bulk review

Make the two-environment model the foundation, not something added at the end:

- **In beta, agents move freely and in small steps.** No approvals, no per-call gates. Beta has its
  own data, synthetic or seeded, captured mail, and service calls that fail unless allowed.
- **Production changes only by promotion.** Beta's changes are bundled into one reviewable diff
  (code, manifests, schema changes, content, and flags) that a person reads and applies in one
  step, and that one step can undo.
- **The review page is the product.** Whether people feel in control rides on how readable that one
  diff is. That is where design effort should go: what changed in reach (new routes, new tables,
  wider grants), what changed in data, and what the last day of beta traffic looked like.

## 5. Make data change cheap until it is promoted

Migrations and long-term compatibility slowed every agent down. A lighter rule:

- In beta, schemas change freely: add, rename, drop. Beta's data can be rebuilt.
- At promotion, the diff classifies each data change (adds a field, adds a table, contracts,
  rewrites). Additions just apply. Anything else needs a migration step in the same promotion,
  which the review shows with a count of affected rows.
- Drop the promise to keep older box versions working against newer schemas. One version runs at a
  time, plus the one being promoted.

## 6. Keep control in the human's hands

- **Write the success criteria first,** in the first human documents, with a date. Each criterion
  should be checkable: a site serves, an agent ships a change through beta to production, a
  backup restores.
- **Give everything a budget:** tools, documents, lines of host code, and tokens per agent team,
  and show where each stands on one page. Going past a budget is a decision for the human, not
  something an agent does on its own.
- **Stop at open decisions.** A feature waiting on an open decision isn't built until the human
  answers. Keep one ledger of open decisions, short enough to read in a minute.
- **Cut regularly.** Every week, list what exists and what nobody used, and delete the unused.
- **I should push back.** When a request would grow the surface, I should say what it costs, offer
  the smaller version, and ask, not just build it fast.

## 7. Worth harvesting

These earned their keep and should carry forward, rewritten small:

- **WASM boxes behind a host-enforced perimeter:** the reactor ABI, fuel, the host minting ids,
  `{!reserved}` identifiers, deny by default (005, 010, 018, 021).
- **Operations:** access logs, traffic and failures as five-minute buckets, the Errors and Box
  failures views, `/goober/ready` health, controls (rate, concurrency, closed) per endpoint, and
  heat studies (028, 032, 047, 048).
- **The limits table:** everything the host grows on disk has a time limit and a byte budget (045).
- **Backups:** sealed nightlies to S3, increments, and rehydrating onto a devbox (019).
- **Logs with a hash chain and an audit log** of every operator action (032).
- **The outbox:** effects delivered after commit, with idempotency keys (015).
- **Identity:** per-domain realms, emailed codes, roles a box may grant through its manifest (006,
  021).
- **Beta and promotion:** the parts of 053 that bundle beta's changes into one change set (042,
  053), as the seed of § 4.
- **Mail in and out:** SES out, Stalwart in, and the operators' mailbox (018, 024, 055).

These were answers to the medium, and the next version should not need them: source projects
and in-host builds (020), skills (050), per-call approvals (035), and most of the operator MCP
tools.

## 8. A smaller first round

A possible first round for the new project, roughly six human documents: the goal and its criteria
with a date, the box and its two manifests, beta and promotion, operations, data, and the harness
agents build in. Build only that, check it against the criteria, and add nothing until the human
reads the result.

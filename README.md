# goober

**The SlopBox Orchestration Service.**

> Status: **design phase.** There is no code yet. The design is being written in
> [`design/`](design/), and implementation choices are made deliberately, one at a time.

## The bet

Goober is a personal research bet on how computing ends up once AI agents are doing
the implementation work. It doesn't wait for a god-level AI. It builds from reasonable
predictions of the capability curve in front of us. If the god shows up, none of this
matters, and that is a clean way for it to die.

Today's idioms quietly assume that the worker is a human who can be trusted with the
keys and moved by incentives. Agents break that assumption. So goober doesn't hand them
the old keys. It gives them **better boxes, with finer scope, and makes the wrong action
impossible rather than merely frowned upon.**

The goal is not for a human to read all the code, or to audit every artifact. The goal
is tooling that lets a person **feel the shape of a box** and the risks of that box going
wrong, and then act. Ironclad proofs sit on the boundary. The perimeter keeps the shape
of a mistake inside a contract: an agent trusted with one user profile cannot open
another, cannot cheat, and cannot delete the universe. Cost control is part of that
contract, because the system controls the fuel going in and out of every box.

Read the specification as a new operating system. Every existing idiom is up for grabs,
but it still has to run on the systems that exist today. Boundaries that earned their
keep, like UNIX, SQLite, and HTTP, are treated as good shapes. The embodiment of a shape
is disposable.

## The shape

- **Storage and compute stay divided.** Fixed-function machines stay more reliable and
  cheaper than a model imitating them.
- **Storage sits above the block** as multi-paradigm engines that are first-class
  citizens. Storage is addressed hierarchically (`storage/sqlite3/$database/$version/$table`)
  so a human can read the address and a machine can enforce it. Time is a first-class
  boundary. SQLite is the tactical start, along with its whole operational burden
  (replication, schema migration, versioning).
- **Processes are ephemeral boxes.** A process wakes up, does a thing, and goes to bed.
  The first box is WASM with no POSIX inheritance. V8 isolates and well-defined
  containers can come later, along with common, predictable boxes the system can
  optimize around.
- **The entrance is shared architecture.** HTTP comes first, so a human reviews the pipe
  rather than the product. Terminals, bi-directional pipes, and other protocols can be
  added as they earn their place.
- **The manifest is the perimeter.** A box is named by a primary key
  (`/process/wasm/<name>`) and is dead until its manifest (`/manifest/wasm/<name>`)
  declares its routes, protocols, data requirements, scopes, session variables, write
  targets, and limits. This is IAM-style authorization that is actually enforced, with
  fuel limits, rate limits, and per-box costs. Agents can veto manifest changes, but
  only a human can approve them.
- **Owning the boundary buys black-box quality control.** Isolation, forking, shadow
  traffic, A/B tests, per-agent staging, audit logs, and monitoring. When an interior is
  bad, you see elevated error rates and latency, not a breach.

The five arenas the design has to contend with are: **naming the box, data flowing in,
data flowing out, interior effects, and process multiplicity and lifetime.**

## Design documents

| Path | Contents |
|---|---|
| [`design/human/`](design/human/) | Documents written by the human. These are authoritative. |
| [`design/human/000.Introduction.md`](design/human/000.Introduction.md) | The introduction and foundational thinking. Start here. |
| [`design/human/005.Goals-And-NonGoals.md`](design/human/005.Goals-And-NonGoals.md) | Goals and non-goals: confidence without reading code, containment over correctness, research over growth. |
| [`design/human/010.Manifest.md`](design/human/010.Manifest.md) | The manifest: routes, signals, the host/guest ABI (reactor pattern), behavior, budgets, and sharing rules. |
| [`design/human/015.Identity.md`](design/human/015.Identity.md) | Identity: OAuth sessions, grant roles, substitution, bootstrapping owners, the login API, and anti-phishing rules. |
| [`design/human/020.DataFlow.md`](design/human/020.DataFlow.md) | Data flow (draft): context, data strategies and access patterns, generated data code, tenants, and SQL vs. solo. |
| [`design/human/025.DataShape.md`](design/human/025.DataShape.md) | Shape of data: storage options and their invariants, sequencers and cache coherency, atomic write sets, and torn-write reporting. |
| [`design/human/030.TheLargerEnvironment.md`](design/human/030.TheLargerEnvironment.md) | The network: external services via OpenAPI, request restrictions, host-held credentials, and box-to-box calls. |
| [`design/human/035.TheOutBox.md`](design/human/035.TheOutBox.md) | The outbox: network side effects captured in the write set and delivered after commit, at least once. |
| [`design/human/036.Json.md`](design/human/036.Json.md) | JSON types: borrowing OpenAPI components.schemas, a strict subset mapped to C, and classified type changes. |
| [`design/human/037.Http.md`](design/human/037.Http.md) | HTTP details: declared parameters, headers, and bodies, the status vocabulary, and what the host does for free. |
| [`design/human/038.Mail.md`](design/human/038.Mail.md) | Mail: Stalwart converts SMTP to routes, verified senders, signed reply addresses, and outbound mail via the outbox. |
| [`design/human/040.TheBus.md`](design/human/040.TheBus.md) | The bus: last-write pub/sub carrying signals not data, coalescing, SSE subscriptions, and broker options. |
| [`design/human/045.Time.md`](design/human/045.Time.md) | Time: host timers and data-driven schedules, with fire semantics. |
| [`design/human/050.AgentsMCP.md`](design/human/050.AgentsMCP.md) | MCP: tools and resources mirroring HTTP, agent-only routes, and agentic considerations. |
| [`design/human/060.Files.md`](design/human/060.Files.md) | Files: uploads, preprocessing, the upload ledger, asset routes, and mark and sweep. |
| [`design/human/064.Keys.md`](design/human/064.Keys.md) | Keys: a vault for the root key only, envelope encryption, per-tenant and per-user keys, and their limits. |
| [`design/human/070.UI.md`](design/human/070.UI.md) | UI: content negotiation, markdown, templates, static content, objects, and the private backing tree. |
| [`design/human/099.TheSystem.md`](design/human/099.TheSystem.md) | The system around: own the box, rent the edge (Caddy, Stalwart, Litestream, NATS) to keep the host small. |
| [`design/machine/`](design/machine/) | Documents drafted by AI. These are proposals until a human ratifies them. |
| [`design/machine/PHILOSOPHY.md`](design/machine/PHILOSOPHY.md) | Engineering philosophy carried over from an earlier project. Not yet ratified. |
| [`design/machine/feedback-2026-10-04.md`](design/machine/feedback-2026-10-04.md) | Long-form outside review: what is unique, what can be bought, limits and proposals, security, the Hearth rebuild, and the blue-collar OS goal. |

This isn't Adama with a new coat of paint, though it does rob that grave for the parts
that still hold.

## Review process

At the end of a working day, Claude writes a dated design review into `design/machine/`, following [`design/REVIEW.PROCESS.md`](design/REVIEW.PROCESS.md).

## License

[MIT](LICENSE) © 2026 Jeffrey M. Barber

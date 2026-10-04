# goober engineering philosophy

This is a living document. It states *why* the code looks the way it does. CLAUDE.md
states the mechanical rules that follow from it.

## 1. No globals. Ever.

No singletons, no mutable statics, no `extern` state, no "service locators", no
static registries that self-populate at load time. Every piece of state is owned by
something, and that something is reachable from exactly one root: `goo::system::System`.

Why:
- **Precision in tests.** A test constructs exactly the world it needs, with a
  manual clock and a temp directory, and nothing leaks between tests.
- **Distributed system in a box.** Because nothing is global, N `System` instances can
  live in one process and talk to each other. Multi-node behavior becomes a unit test.
- **Honest dependencies.** If code needs something, it says so in its signature.

Corollary: time, randomness, the filesystem root, and the network are all *injected*.
Reading `std::chrono::steady_clock::now()` directly is a global read; take a
`core::Clock&` instead.

## 2. The god object is explicit

`System` owns every subsystem and is passed explicitly (`System&`) to code that
needs broad access, e.g. HTTP handlers. Access reads like a path:
`system.database->sqlite->find_or_create("name")`.

But being reachable from `System` doesn't mean depending on `System`:
- **Leaf packages** (`core`, `config`, `http`, `data/*`) never include `system/`.
  They take only the narrow things they need (a `Clock&`, a `Config` struct, a
  `Handler`). This keeps them reusable and testable in isolation.
- **Composition packages** (`system`, `api`) wire leaves together and may take `System&`.

## 3. Clean boundaries, enforced by the build

Each directory under `src/` is a package, a namespace, and a static library.
`src/data/sqlite3` is `goo::data::sqlite3` is `goo_data_sqlite3`. A package lists
its dependencies in its `CMakeLists.txt`; if it isn't listed, it can't be included.
Dependency direction flows one way: leaves -> composition -> `main`.

Phases are coming (single-server MVP first, distributed later). Boundaries are what
make phases cheap: a later phase should replace an implementation behind a package
boundary rather than rewrite callers.

## 4. Convention over configuration, with overrides

The binary takes exactly one argument: a config file. No flags, no env-var
side channels. Every setting has a conventional default derived from something
simpler (e.g. `sqlite.directory` defaults to `<paths.data>/sqlite`, which defaults to
`<paths.root>/data`, which defaults to the config file's directory). A config file
states only what differs from convention. Unknown keys are errors, because a typo'd
override that is silently ignored is worse than a crash.

Packages define their own plain `Config` structs with defaults. Only the composition
root (`system/config.cpp`) knows the file format, so leaves stay format-agnostic.

## 5. Lean on well-engineered libraries; write less

Prefer a proven library over bespoke code: Boost (Asio, Beast, JSON), SQLite,
GoogleTest. Before writing a nontrivial utility, look for a good package and note the
decision. Wrappers around libraries are thin and exist to impose *our* boundaries
(RAII, error types, naming), not to re-abstract the library.

## 6. Reasonable tests, not exhaustive tests

Test behavior at package boundaries: the things a caller relies on, and the edge
cases that would hurt (path traversal in names, idle-unload rules, config typos).
Don't test the library we're wrapping, and don't write tests that merely restate
the implementation. Tests are deterministic: manual clocks, ephemeral ports, unique
temp dirs, no sleeps.

## 7. Serialization is a first-class concern (TBD)

Wire and storage formats will outlive the code that writes them. Serialization will
get its own package and its own rules (versioning, compatibility, determinism) before
we commit to a binary protocol. Until then: JSON for config and debug endpoints only;
don't let ad-hoc serialization leak into package APIs.

## 8. Built for AI-assisted development

Most code here will be written with an AI pair (Claude, Grok). That favors:
- predictable structure (directory == namespace == library),
- mainstream, well-documented tools (CMake, Boost, GoogleTest) over niche ones,
- small files with one clear responsibility,
- documentation of *why* in this file and *how* in CLAUDE.md, kept current.

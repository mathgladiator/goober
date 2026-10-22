# goober host

The first goober host (v1): one machine that serves static content per tenant, runs WASM
boxes behind HTTP routes, and is operated over MCP. The decisions behind it are in
[`../design/machine/005.FirstVersion.md`](../design/machine/005.FirstVersion.md).

## Build and test

Dependencies live under `~/.local`: Boost 1.83+, GoogleTest, and wasmtime v49.0.4
(`tools/install-deps.sh` fetches wasmtime). Then:

```sh
cmake --preset dev          # configure (Ninja, Debug, build/dev)
cmake --build --preset dev  # incremental build
ctest --preset dev          # every package's tests
```

## Run and play

From the repo root, with [`just`](https://github.com/casey/just):

```sh
just setup   # interactive: dev box or production, domains, owners, operators, threads, limits
just run     # build, then run demo/ in local mode (dev defaults if there is no config yet)
just mcp     # mint an operator token and print the `claude mcp add` command
```

Then open http://localhost:8080/. `just reload` rereads the config; Ctrl-C stops the host.
Everything the host owns is under `demo/storage/`, readable by a person and by a backup
tool. Any directory works in place of `demo`: `just setup /srv/goober`, `just run /srv/goober`.

**Local mode** (`--local`, which `just run` uses) listens on 127.0.0.1, uses plain session
cookies, prints sign-in codes in the terminal, and lets `$domain.localhost` reach any
configured `$domain`, so a production config can be played on a laptop.

### Identity

Sign in at `/identity/sign-in` on any domain with an emailed six-digit code; in local mode
the code is printed by the host. `/identity/account` shows who you are and your roles. The
address set as a domain's `owner` becomes `owner` of that tenant's realm at sign-in, and the
addresses in `identity.operators` can sign in as host operators. Every route declares
`"public": true` or `"requirements"`, and the box receives `user` in its request document.
See [`006.LocalIdentity.md`](../design/machine/006.LocalIdentity.md).

### Operating it over MCP

The operator endpoint is `/goober-operations-mcp` (MCP over streamable HTTP) with a bearer
token: one minted by `just mcp` (stored only as a hash), or `operations.token` from the
config on a dev box.

| tool | does |
| --- | --- |
| content_put, content_get, content_list, content_delete | Static files per tenant (`_host` for host-wide ones like `error-message/404.html`). |
| version_upload | A manifest plus WAT or base64 WASM; checked, stored, and made active. |
| version_list, version_get, version_promote | Versions of a box; promote is rollback. |
| realm_get, role_grant, role_revoke, sessions_revoke | A tenant realm's users and roles. |
| box_list, box_logs, access_log, host_health, domain_list | Looking around. |

`tools/goober.py` is a stdlib-only client for the same endpoint:

```sh
tools/goober.py tools          # uses demo/host.json, or GOOBER_CONFIG
tools/goober.py call content_put tenant=playground path=hi.txt text=hello
tools/goober.py call role_grant tenant=playground email=friend@example.com role=admin
tools/goober.py deploy examples/boxes/echo/manifest.json examples/boxes/echo/echo.wat
tools/goober.py push playground examples/site
```

## Writing a box

A box is a WASM module that imports only from module `goober` and exports `memory` plus
one function per route, `goober_route_$name`, taking nothing and returning i32 (0 = ran).

| import | signature |
| --- | --- |
| goober_request_size | () -> i32 |
| goober_request_read | (ptr, len) -> i32 |
| goober_respond | (status, type_ptr, type_len, body_ptr, body_len) -> i32 |
| goober_header_set | (name_ptr, name_len, value_ptr, value_len) -> i32 |
| goober_now_ms | () -> i64, fixed per request |
| goober_random_fill | (ptr, len) -> i32, seeded per request; the seed is in the access log |
| goober_log | (level, ptr, len) -> i32, written to logs/boxes/$box.log after the call |

The request document is JSON: `request_id`, `method`, `path`, `params`, `query`,
`headers` (without authorization and cookie), `body`, `domain`, `tenant`, and `user`
(null, or `user_id`, `email`, `roles`, `host`).

- `examples/boxes/echo` is hand-written WAT that returns the request document, with
  `/whoami` (any signed-in user) and `/owners-room` (`owner` or `host:owner`).
- `examples/boxes/dice` is Rust (`cargo build --release --target wasm32-unknown-unknown`)
  with a small binding in `src/goober.rs`.

## Layout

Each package under `packages/` is one directory, one library (`goober_$name`), one test
binary, and a `QUALITY.md`. The underscore in a name is a namespace (`server_http` is
`goober::server::http`).

| package | is |
| --- | --- |
| base | Hashes, encodings, the name grammar, durable files, seeded randomness, time. |
| json | Boost.JSON, compiled once, with typed field readers. |
| config | The JSON host config, validation, and reload. |
| wal | The host WAL: bundles of file changes, committed then applied, replayed after a crash. |
| content | Static content per tenant, through the WAL. |
| router | Path normalization, route patterns, and the route table. |
| manifest | The v1 manifest subset, its checks, and route requirements. |
| runtime_wasm | wasmtime with fuel, epoch timeouts, memory caps, and the goober ABI. |
| boxes | Box versions, activation, and per-domain route tables. |
| identity | Sign-in codes, sessions, roles, and the audit, in one SQLite database per realm. |
| server_http | The HTTP/1.1 server on Boost.Beast. |
| mcp | MCP over streamable HTTP: JSON-RPC and a tool registry. |
| host | The wiring, identity endpoints, operator tools and tokens, and the `goober-host` binary. |

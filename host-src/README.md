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

```sh
build/dev/packages/host/goober-host --config dev/host.json   # serves http://127.0.0.1:8080
tools/play.sh        # in another shell: builds and deploys the example boxes and site
```

Then open http://localhost:8080/. `kill -HUP` rereads the config; Ctrl-C stops the host.
Everything the host owns is under `dev/storage/`, readable by a person and by a backup tool.

### Operating it over MCP

The operator endpoint is `/goober-operations-mcp` (MCP over streamable HTTP), with the
bearer token from `operations.token`. To give Claude Code the tools:

```sh
claude mcp add --transport http goober http://127.0.0.1:8080/goober-operations-mcp \
  --header "Authorization: Bearer dev-token-change-me-please"
```

| tool | does |
| --- | --- |
| content_put, content_get, content_list, content_delete | Static files per tenant (`_host` for host-wide ones like `error-message/404.html`). |
| version_upload | A manifest plus WAT or base64 WASM; checked, stored, and made active. |
| version_list, version_get, version_promote | Versions of a box; promote is rollback. |
| box_list, box_logs, access_log, host_health, domain_list | Looking around. |

`tools/goober.py` is a stdlib-only client for the same endpoint:

```sh
tools/goober.py tools
tools/goober.py call content_put tenant=playground path=hi.txt text=hello
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
`headers` (without authorization and cookie), `body`, `domain`, and `tenant`.

- `examples/boxes/echo` is hand-written WAT that returns the request document.
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
| manifest | The v1 manifest subset and its checks. |
| runtime_wasm | wasmtime with fuel, epoch timeouts, memory caps, and the goober ABI. |
| boxes | Box versions, activation, and per-domain route tables. |
| server_http | The HTTP/1.1 server on Boost.Beast. |
| mcp | MCP over streamable HTTP: JSON-RPC and a tool registry. |
| host | The wiring, the operator tools, and the `goober-host` binary. |

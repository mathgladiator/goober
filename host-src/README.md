# goober host

The first goober host (v1): one machine that serves sites per tenant as a CMS, runs WASM
boxes behind HTTP routes with their own tenant databases, and is operated over MCP. The
decisions behind it are in [`005.FirstVersion.md`](../design/machine/005.FirstVersion.md),
[`006.LocalIdentity.md`](../design/machine/006.LocalIdentity.md),
[`007.Content.md`](../design/machine/007.Content.md), and
[`008.Databases.md`](../design/machine/008.Databases.md).

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
just examples  # deploy the example boxes, the demo site, and its images to the running host
just db        # every tenant's databases; just sql playground guestbook "SELECT * FROM entries"
just terminal-e2e  # play the dumb mobile terminal and an agent against the running host
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
addresses in `identity.operators` can sign in as host operators at `/identity/sign-in/as-host`,
which nothing links to. Every route declares
`"public": true` or `"requirements"`, and the box receives `user` in its request document.
See [`006.LocalIdentity.md`](../design/machine/006.LocalIdentity.md).

### Sites: pages, templates, listings, images

Each tenant's content tree is a site ([`007.Content.md`](../design/machine/007.Content.md)):

- **Pages** are `.md` (markdown), `.html` with front matter (a fragment, wrapped in a
  template), or `.html` without it (a whole document, served as written). `blog/hello.md`
  is `/blog/hello`; `blog/index.md` is `/blog`.
- **Templates** are Mustache in `_templates/$name.html`, with layouts (`layout: base` in a
  template's front matter) and partials (`{{> nav}}`). Built-in `base`, `page`, and `list`
  templates make a site look clean with none of its own.
- **Listings**: a page with `list: blog` lists the pages in `blog/`, sorted, paged
  (`?page=2`), filtered (`?tag=x`), and with `feed: true` an Atom feed. A directory without
  an index is listed only when a `directory` template exists.
- **Site data**: `_data/site.json` (title, nav, footer, stylesheet) and any `_data/*.json`.
  Paths with a segment starting with `_` are never served.
- **Attachments** are images and PDFs at `/attachment/$id.$ext`: `attachment_put` over MCP
  (base64, or an https URL to fetch), `POST /attachment/upload` with a token or as an owner,
  or the manager page at `/attachment/`.

`just examples` deploys a demo site with all of these, and its home page shows an image
imported from https://jeffrey.io/attachment/1.png.

### Databases

A box declares `data.sqlite3.$strategy` with a stored schema, a database name, and what it
may touch; each tenant gets its own SQLite file, created on first use and only ever grown by
schemas ([`008.Databases.md`](../design/machine/008.Databases.md)). Operators look in with
`db_list`, `db_describe`, `db_query`, and `db_exec` over MCP, or from the shell:

```sh
just db                                                  # tenants, databases, tables, rows
just db-describe playground guestbook                    # columns, indexes, schemas applied
just sql playground guestbook "SELECT * FROM entries"    # read-only
just sql-exec playground guestbook "DELETE FROM entries WHERE id = 3"   # logged to logs/db.log
```

### Operating it over MCP

The operator endpoint is `/goober-operations-mcp` (MCP over streamable HTTP) with a bearer
token: one minted by `just mcp` (stored only as a hash), or `operations.token` from the
config on a dev box.

| tool | does |
| --- | --- |
| site_guide | How sites work, for an agent to read first. |
| page_put, page_get, page_list, page_render | Pages by URL, as markdown, fragment, or document; render previews drafts. |
| template_put, template_list | Mustache templates, checked before they are stored. |
| attachment_put, attachment_list, attachment_get, attachment_delete | Images and PDFs, with what uses each one. |
| content_put, content_get, content_list, content_delete | Any file per tenant (`_host` for host-wide ones like `error-message/404.html`). |
| schema_put, schema_get, schema_list | Database schemas, stored once by name. |
| db_list, db_describe, db_query, db_exec | Every tenant's databases: what uses them, what's in them, and changing rows. |
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
tools/goober.py attach playground https://jeffrey.io/attachment/1.png --alt "A picture"
tools/goober.py schema examples/boxes/guestbook/guestbook1.schema.json
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
| goober_sql | (strategy_ptr, strategy_len, sql_ptr, sql_len, params_ptr, params_len) -> i32, the result's length |
| goober_result_read | (ptr, len) -> i32, copies the last result |

The request document is JSON: `request_id`, `method`, `path`, `params`, `query`,
`headers` (without authorization and cookie), `body`, `domain`, `tenant`, and `user`
(null, or `user_id`, `email`, `roles`, `host`).

- `examples/boxes/echo` is hand-written WAT that returns the request document, with
  `/whoami` (any signed-in user) and `/owners-room` (`owner` or `host:owner`).
- `examples/boxes/dice` is Rust (`cargo build --release --target wasm32-unknown-unknown`)
  with a small binding in `src/goober.rs`.
- `examples/boxes/guestbook` is Rust with a database: SQLite builds its JSON response, and
  the route's template renders it inside the site's layout for a browser.
- `examples/boxes/terminal` and `examples/boxes/desk` are the backend of the dumb mobile
  terminal (`design/machine/010.Terminal.md`): the person's routes, and the agents' routes
  (`"agents": true`), over one database, sharing the serde binding in `examples/boxes/shared/`.

## Deploy to a server

On a fresh Ubuntu 24.04+ or Debian 13+ machine (x86_64, sudo, DNS already pointing at it):

```sh
just deploy you@server            # or host-src/tools/deploy.sh you@server [/srv/goober/host]
just deploy you@server --with-rust  # also Rust and vendored crates, to build source projects there
just deploy server --user customer   # ssh in as customer (the same as customer@server)
just deploy you@server --run-as acme # the host runs as the system user acme, in /srv/acme/host
```

It builds the `server` preset here (a release binary with libstdc++ linked in, needing only
glibc 2.38), copies it, and runs `tools/server-bootstrap.sh` there. That script installs Caddy,
curl, python3, tzdata, and bubblewrap (the build sandbox); creates a `goober` system user and `/srv/goober/host`; runs setup as
`goober` (answer **Production**); and installs the systemd unit, a nightly backup timer (two
weeks kept, in `/srv/goober/host/backups/`), and the Caddyfile, which gets certificates on its
own. Running it again upgrades: it checks the config with the new binary and restarts.

`you` in `you@server` (or `--user you`) is your ssh login, which needs sudo; ssh connects once,
so a password is asked for once. `--run-as NAME` is a different account: the system user the
host runs as and owns its files (default `goober`, home `/srv/NAME`, so the
default directory is `/srv/NAME/host`). An existing account is used only if it is a system user
whose home is `/srv/NAME`, never root or a person's login. The examples here use the default.

Then, on the server, mint an operator token
(`sudo -u goober goober-host token create --config /srv/goober/host/host.json --name claude-code --claude`)
and use it from your laptop: `GOOBER_URL=https://your.domain GOOBER_TOKEN=... tools/goober.py push playground site/`.

For mail to reach anyone but you, answer yes to Amazon SES in setup (with a verified sending
address, and your account out of SES's sandbox), or to SMTP with its password in a file the
`goober` user can read (`chown root:goober`, `chmod 0640`).

For backups off the machine, give setup an S3 bucket. Setup stores the AWS keys in the key
store (`goober-host key set aws_access_key_id`, value on stdin) and prints the **backup key**:
keep a copy somewhere else, then run `goober-host backup key --confirm`. On your laptop,
`just rehydrate ~/copy` rebuilds any moment as a sandboxed devbox
(`design/machine/019.Backups.md`).

## Source projects

Agents keep a product's source in the host and build and deploy it from there
(`design/machine/020.Source.md`): `source_create`, `source_checkout`, `source_commit`,
`source_deploy`. People use the same projects from a terminal:

```sh
tools/goober.py source new hello ~/src/hello rust-box   # create and pull
$EDITOR ~/src/hello/src/lib.rs
tools/goober.py source push ~/src/hello "Say more"       # commit what changed
tools/goober.py source deploy hello                      # build in the sandbox, deploy
```

Rust builds need cargo with the wasm32 target, bubblewrap, and vendored crates
(`tools/vendor.sh DIR`, then `"source": {"vendor": "DIR"}`); `just setup` does this on a dev box.

## Layout

Each package under `packages/` is one directory, one library (`goober_$name`), one test
binary, and a `QUALITY.md`. The underscore in a name is a namespace (`server_http` is
`goober::server::http`).

| package | is |
| --- | --- |
| base | Hashes (SHA-256, HMAC), ChaCha20, encodings, the name grammar, durable files, processes, seeded randomness, time. |
| json | Boost.JSON, compiled once, with typed field readers. |
| aws | Signature Version 4, S3 objects, and SES mail, through curl. |
| backup | Sealed files, page-level SQLite deltas, and the names of what goes in the bucket. |
| config | The JSON host config, validation, and reload. |
| wal | The host WAL: bundles of file changes, committed then applied, replayed after a crash. |
| render | Front matter, Mustache, and markdown to HTML. |
| mail_templates | The templates every mail is rendered from: defaults, per-tenant overrides, and the layout. |
| sqlite | A small RAII wrapper over SQLite. |
| content | The content tree per tenant, through the WAL. |
| attachments | Byte-sniffed images and PDFs, with a ledger per tenant. |
| site | Pages, templates, listings, and feeds from a content tree. |
| router | Path normalization, route patterns, and the route table. |
| manifest | The v1 manifest subset, its checks, and route requirements. |
| runtime_wasm | wasmtime with fuel, epoch timeouts, memory caps, and the goober ABI. |
| db | Schemas, tenant databases, the SQL authorizer, request transactions, and operator queries. |
| boxes | Box versions, activation, and per-domain route tables. |
| identity | Sign-in codes, sessions, roles, and the audit, in one SQLite database per realm. |
| server_http | The HTTP/1.1 server on Boost.Beast. |
| mcp | MCP over streamable HTTP: JSON-RPC and a tool registry. |
| source | Source projects: revisions, conflicts, and unified diffs. |
| host | The wiring, identity endpoints, operator tools and tokens, and the `goober-host` binary. |

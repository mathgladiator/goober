# goober: build, set up, run, and operate a host. `just` lists the recipes.

set shell := ["bash", "-euo", "pipefail", "-c"]

host_src := justfile_directory() / "host-src"
bin := host_src / "build/dev/packages/host/goober-host"

# The host directory. Give it as the first argument (`just mcp demo2`), as `dir=demo2`
# after the recipe, or as `just dir=demo2 mcp`.
dir := "demo"

# List the recipes.
default:
    @just --list --unsorted

# Configure (once) and build the host incrementally.
build:
    @cd {{host_src}} && { [ -f build/dev/build.ninja ] || cmake --preset dev >/dev/null; } && cmake --build --preset dev

# Build and run every package's tests.
test: build
    cd {{host_src}} && ctest --preset dev

# Build, set up DIR with dev defaults if it has no config, and run the host in local mode.
run dir=dir: build
    @[ -f {{trim_start_match(dir, "dir=")}}/host.json ] || {{host_src}}/tools/setup.sh --defaults {{trim_start_match(dir, "dir=")}}
    exec {{bin}} --config {{trim_start_match(dir, "dir=")}}/host.json --local

# Build and run the host exactly as configured (no local mode), as production would.
serve dir=dir: build
    exec {{bin}} --config {{trim_start_match(dir, "dir=")}}/host.json

# Walk through the core decisions and write DIR/host.json (a dev box or production).
setup dir=dir: build
    {{host_src}}/tools/setup.sh {{trim_start_match(dir, "dir=")}}

# Mint an operator MCP token and print the command that registers it with Claude Code.
mcp dir=dir name="claude-code": build
    #!/usr/bin/env bash
    set -euo pipefail
    # Arguments may also be given as dir=... and name=..., in either order.
    d='{{dir}}' n='{{name}}'
    case "$d" in *=*) d=demo;; esac
    case "$n" in *=*) n=claude-code;; esac
    for a in '{{dir}}' '{{name}}'; do case "$a" in dir=*) d="${a#dir=}";; name=*) n="${a#name=}";; esac; done
    [ -f "$d/host.json" ] || { echo "no $d/host.json yet: run just setup (or just run) first" >&2; exit 1; }
    cmd=$('{{bin}}' token create --config "$d/host.json" --name "$n" --claude)
    echo "Minted operator token '$n' (host:owner). Only its hash is kept, in $d/storage/operations/tokens/."
    echo "Register it with Claude Code by running:"
    echo
    echo "$cmd"
    echo
    echo "List tokens with: just tokens    Revoke one with: just revoke <name-or-id>"

# List the operator tokens minted for DIR.
tokens dir=dir: build
    @{{bin}} token list --config {{trim_start_match(dir, "dir=")}}/host.json

# Revoke operator tokens by name or id.
revoke which dir=dir: build
    @{{bin}} token revoke {{which}} --config {{trim_start_match(dir, "dir=")}}/host.json

# Deploy the example boxes and site to the running host for DIR.
examples dir=dir: build
    {{host_src}}/tools/examples.sh {{trim_start_match(dir, "dir=")}}/host.json

# Play the dumb terminal and an agent against the running host for DIR, end to end (999).
terminal-e2e dir=dir url="http://localhost:8080":
    python3 -I {{host_src}}/tools/terminal_e2e.py --config {{trim_start_match(dir, "dir=")}}/host.json --url {{trim_start_match(url, "url=")}}

# List every tenant's databases, their tables, and row counts.
db dir=dir: build
    @{{bin}} db list --config {{trim_start_match(dir, "dir=")}}/host.json

# Describe one tenant database: tables, columns, indexes, and applied schemas.
db-describe tenant database dir=dir: build
    @{{bin}} db describe {{quote(tenant)}} {{quote(database)}} --config {{trim_start_match(dir, "dir=")}}/host.json

# Run one read-only SQL statement on a tenant database, like: just sql playground guestbook "SELECT * FROM entries"
sql tenant database query dir=dir: build
    @{{bin}} db query {{quote(tenant)}} {{quote(database)}} {{quote(query)}} --config {{trim_start_match(dir, "dir=")}}/host.json

# Run one INSERT, UPDATE, or DELETE on a tenant database (logged to logs/db.log).
sql-exec tenant database query dir=dir: build
    @{{bin}} db exec {{quote(tenant)}} {{quote(database)}} {{quote(query)}} --config {{trim_start_match(dir, "dir=")}}/host.json

# A consistent copy of DIR's storage into DEST, safe while the host runs (logs and mail left out).
backup dest dir=dir: build
    @{{bin}} backup {{quote(dest)}} --config {{trim_start_match(dir, "dir=")}}/host.json

# Copy a backup into DIR's storage, which must be empty; stop the host first.
restore from dir=dir: build
    @{{bin}} restore {{quote(from)}} --config {{trim_start_match(dir, "dir=")}}/host.json

# Rebuild a sandboxed devbox in DEST from the S3 backups, at the latest moment or one like
# 2026-10-26T18:00 (UTC). Reads ~/.config/goober/backup.env; see host-src/tools/rehydrate.sh.
rehydrate dest at="": build
    {{host_src}}/tools/rehydrate.sh {{quote(dest)}} {{quote(at)}}

# S3 backups end to end against a stand-in S3: nightly, increment, assets, rehydrate.
backup-e2e: build
    python3 -I {{host_src}}/tools/backup_e2e.py

# Fuzz every parser and the SQL authorizer under AddressSanitizer: just fuzz 50000
fuzz iters="20000":
    cmake --preset asan >/dev/null && cmake --build --preset asan
    ASAN_OPTIONS=detect_leaks=0 GOOBER_FUZZ_ITERS={{iters}} {{host_src}}/build/asan/packages/fuzz/goober_fuzz_test

# Check DIR/host.json without starting anything.
check dir=dir: build
    @{{bin}} check --config {{trim_start_match(dir, "dir=")}}/host.json

# Tell the running host for DIR to reread its config (SIGHUP).
reload dir=dir:
    @pkill -HUP -f -- "[g]oober-host --config {{trim_start_match(dir, "dir=")}}/host.json" && echo "reloaded" || echo "no running host for {{trim_start_match(dir, "dir=")}}"

# Install or upgrade goober on a server over ssh (Ubuntu 24.04+ / Debian 13+), with Caddy and backups.
# Extra arguments pass through: a DIR, --user LOGIN (ssh), --run-as NAME (the host's account), --with-rust.
deploy target *args:
    {{host_src}}/tools/deploy.sh {{target}} {{args}}

# Play the example products (leads, ranches) end to end against the running host, as GrokBot would.
products-e2e dir=dir url="http://localhost:8080":
    python3 -I {{host_src}}/tools/products_e2e.py --config {{trim_start_match(dir, "dir=")}}/host.json --url {{trim_start_match(url, "url=")}}

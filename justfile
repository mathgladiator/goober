# goober: build, set up, run, and operate a host. `just` lists the recipes.

set shell := ["bash", "-euo", "pipefail", "-c"]

host_src := justfile_directory() / "host-src"
bin := host_src / "build/dev/packages/host/goober-host"

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
run dir="demo": build
    @[ -f {{dir}}/host.json ] || {{host_src}}/tools/setup.sh --defaults {{dir}}
    exec {{bin}} --config {{dir}}/host.json --local

# Build and run the host exactly as configured (no local mode), as production would.
serve dir="demo": build
    exec {{bin}} --config {{dir}}/host.json

# Walk through the core decisions and write DIR/host.json (a dev box or production).
setup dir="demo": build
    {{host_src}}/tools/setup.sh {{dir}}

# Mint an operator MCP token and print the command that registers it with Claude Code.
mcp dir="demo" name="claude-code": build
    @[ -f {{dir}}/host.json ] || { echo "no {{dir}}/host.json yet: run just setup (or just run) first"; exit 1; }
    @echo "Minted operator token '{{name}}' (host:owner). Only its hash is kept, in {{dir}}/storage/operations/tokens/."
    @echo "Register it with Claude Code by running:"
    @echo
    @{{bin}} token create --config {{dir}}/host.json --name {{name}} --claude
    @echo
    @echo "List tokens with: just tokens {{dir}}    Revoke one with: just revoke <name-or-id> {{dir}}"

# List the operator tokens minted for DIR.
tokens dir="demo": build
    @{{bin}} token list --config {{dir}}/host.json

# Revoke operator tokens by name or id.
revoke which dir="demo": build
    @{{bin}} token revoke {{which}} --config {{dir}}/host.json

# Deploy the example boxes and site to the running host for DIR.
examples dir="demo": build
    {{host_src}}/tools/examples.sh {{dir}}/host.json

# Check DIR/host.json without starting anything.
check dir="demo": build
    @{{bin}} check --config {{dir}}/host.json

# Tell the running host for DIR to reread its config (SIGHUP).
reload dir="demo":
    @pkill -HUP -f -- "[g]oober-host --config {{dir}}/host.json" && echo "reloaded" || echo "no running host for {{dir}}"

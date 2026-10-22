#!/usr/bin/env bash
# Builds the example boxes and deploys them, with the example site, to a running host.
#
#   tools/examples.sh CONFIG [TENANT]
#
# The token is GOOBER_TOKEN, else the config's operations.token, else a token minted
# for this run and revoked after it.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
HOST_SRC="$(dirname "$HERE")"
BIN="${GOOBER_HOST_BIN:-$HOST_SRC/build/dev/packages/host/goober-host}"
CONFIG="${1:?usage: examples.sh CONFIG [TENANT]}"
export GOOBER_CONFIG="$CONFIG"
TENANT="${2:-$(python3 -c 'import json,sys; print(next(iter(json.load(open(sys.argv[1]))["domains"].values()))["tenant"])' "$CONFIG")}"

if [[ -z "${GOOBER_TOKEN:-}" ]] && ! python3 -c 'import json,sys; sys.exit(0 if json.load(open(sys.argv[1])).get("operations",{}).get("token") else 1)' "$CONFIG"; then
  GOOBER_TOKEN="$("$BIN" token create --config "$CONFIG" --name examples)"
  export GOOBER_TOKEN
  trap '"$BIN" token revoke examples --config "$CONFIG" >/dev/null' EXIT
fi

if command -v cargo >/dev/null; then
  cargo build --quiet --release --target wasm32-unknown-unknown --manifest-path "$HOST_SRC/examples/boxes/dice/Cargo.toml"
  "$HERE/goober.py" deploy "$HOST_SRC/examples/boxes/dice/manifest.json" \
    "$HOST_SRC/examples/boxes/dice/target/wasm32-unknown-unknown/release/dice.wasm" >/dev/null
  echo "deployed dice"
else
  echo "no cargo: skipping the Rust dice box"
fi
"$HERE/goober.py" deploy "$HOST_SRC/examples/boxes/echo/manifest.json" "$HOST_SRC/examples/boxes/echo/echo.wat" >/dev/null
echo "deployed echo"
"$HERE/goober.py" push "$TENANT" "$HOST_SRC/examples/site"

#!/usr/bin/env bash
# Builds the example boxes and deploys them, with the example site and its images, to a
# running host.
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

EX="$HOST_SRC/examples"
for schema in guestbook/guestbook1 terminal/terminal1 leads/leads2 ranches/ranches1; do
  "$HERE/goober.py" schema "$EX/boxes/$schema.schema.json" >/dev/null
  echo "stored schema ${schema#*/}"
done
if command -v cargo >/dev/null; then
  for box in dice guestbook terminal desk leads ranches; do
    cargo build --quiet --release --target wasm32-unknown-unknown --manifest-path "$EX/boxes/$box/Cargo.toml"
    "$HERE/goober.py" deploy "$EX/boxes/$box/manifest.json" "$EX/boxes/$box/target/wasm32-unknown-unknown/release/$box.wasm" >/dev/null
    echo "deployed $box"
  done
else
  echo "no cargo: skipping the Rust boxes (dice, guestbook, terminal, desk, leads, ranches)"
fi
# The sample landing page posts to the beef-share campaign, which the owner opens: the first
# request makes the leads database, then the campaign goes in as the operator.
if command -v cargo >/dev/null; then
  PORT="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("listen",{}).get("port",8080))' "$CONFIG")"
  curl -s -o /dev/null -X POST -H 'content-type: application/json' --data '{}' "http://127.0.0.1:$PORT/leads/beef-share/signup" || true
  "$HERE/goober.py" call db_exec tenant="$TENANT" database=leads \
    sql="INSERT INTO campaigns (campaign, title, open, created_ms, updated_ms) VALUES ('beef-share', 'The beef share landing page', 1, 0, 0) ON CONFLICT (campaign) DO NOTHING" >/dev/null &&
    echo "opened the beef-share campaign"
fi
"$HERE/goober.py" deploy "$EX/boxes/echo/manifest.json" "$EX/boxes/echo/echo.wat" >/dev/null
echo "deployed echo"

# The site's hero image: imported from jeffrey.io, or the bundled SVG without a network.
HERO="$("$HERE/goober.py" attach "$TENANT" https://jeffrey.io/attachment/1.png --name jeffrey.io-1.png \
          --alt "A picture from jeffrey.io" 2>/dev/null)" ||
  HERO="$("$HERE/goober.py" attach "$TENANT" "$EX/assets/goober.svg" --name goober.svg --alt "goober")"
echo "hero image at $HERO"
# The home page moved from index.html to index.md; drop the old one if an earlier run left it.
"$HERE/goober.py" call content_delete tenant="$TENANT" path=index.html >/dev/null 2>&1 || true
"$HERE/goober.py" push "$TENANT" "$EX/site" HERO_IMAGE="$HERO"

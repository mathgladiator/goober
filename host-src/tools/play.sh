#!/usr/bin/env bash
# Builds the host and the example boxes, then deploys the examples to a running
# host (start it first with: build/dev/packages/host/goober-host --config dev/host.json).
set -euo pipefail
cd "$(dirname "$0")/.."
cmake --preset dev >/dev/null
cmake --build --preset dev
if command -v cargo >/dev/null || [ -x "$HOME/.local/bin/cargo" ]; then
  PATH="$HOME/.local/bin:$PATH" cargo build --quiet --release --target wasm32-unknown-unknown \
    --manifest-path examples/boxes/dice/Cargo.toml
  tools/goober.py deploy examples/boxes/dice/manifest.json \
    examples/boxes/dice/target/wasm32-unknown-unknown/release/dice.wasm
fi
tools/goober.py deploy examples/boxes/echo/manifest.json examples/boxes/echo/echo.wat
tools/goober.py push playground examples/site
echo "open http://localhost:8080/"

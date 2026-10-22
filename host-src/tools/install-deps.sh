#!/usr/bin/env bash
# Installs the pinned third-party dependencies that are not already under ~/.local.
# Boost (1.83+), GoogleTest, and SQLite are expected to be present already.
set -euo pipefail
PREFIX="${GOOBER_LOCAL:-$HOME/.local}"
WASMTIME_VERSION="v49.0.4"
NAME="wasmtime-${WASMTIME_VERSION}-x86_64-linux-c-api"
mkdir -p "$PREFIX/opt"
if [ ! -f "$PREFIX/opt/$NAME/lib/libwasmtime.a" ]; then
  echo "installing $NAME into $PREFIX/opt"
  curl -sSL "https://github.com/bytecodealliance/wasmtime/releases/download/${WASMTIME_VERSION}/${NAME}.tar.xz" \
    | tar -xJ -C "$PREFIX/opt"
fi
for f in include/boost/version.hpp lib/cmake/GTest/GTestConfig.cmake; do
  [ -f "$PREFIX/$f" ] || echo "missing: $PREFIX/$f"
done
echo "dependencies ready"

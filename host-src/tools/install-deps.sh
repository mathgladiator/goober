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
# OpenSSL's libcrypto, static, for Web Push (design/machine/782.PWA.md). Checked against its
# published SHA-256 before it is built.
OPENSSL_VERSION="3.5.1"
OPENSSL_SHA256="529043b15cffa5f36077a4d0af83f3de399807181d607441d734196d889b641f"
if [ ! -f "$PREFIX/opt/openssl-$OPENSSL_VERSION/lib/libcrypto.a" ]; then
  echo "building openssl-$OPENSSL_VERSION into $PREFIX/opt (a few minutes)"
  WORK="$(mktemp -d)"
  curl -fsSL -o "$WORK/openssl.tar.gz" \
    "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz"
  echo "$OPENSSL_SHA256  $WORK/openssl.tar.gz" | sha256sum -c --quiet
  tar -xzf "$WORK/openssl.tar.gz" -C "$WORK"
  (cd "$WORK/openssl-$OPENSSL_VERSION" &&
    ./Configure linux-x86_64 no-shared no-tests no-docs no-apps no-module no-legacy no-engine \
      --prefix="$PREFIX/opt/openssl-$OPENSSL_VERSION" --libdir=lib >/dev/null &&
    make -j"$(nproc)" >/dev/null && make install_sw >/dev/null)
  rm -rf "$WORK"
fi
for f in include/boost/version.hpp lib/cmake/GTest/GTestConfig.cmake; do
  [ -f "$PREFIX/$f" ] || echo "missing: $PREFIX/$f"
done
echo "dependencies ready"

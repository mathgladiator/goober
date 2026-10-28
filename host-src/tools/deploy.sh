#!/usr/bin/env bash
# Deploys goober to a server over ssh: builds the server binary here, copies it, and runs
# tools/server-bootstrap.sh there (Caddy, a goober user, setup, systemd units, backups).
#
#   tools/deploy.sh user@server [DIR]
#
# DIR is the host directory on the server (default /srv/goober/host). The first run walks
# through setup interactively on the server; later runs replace the binary and restart.
# The server needs Ubuntu 24.04 or newer, or Debian 13 or newer (glibc 2.38), x86_64, and
# sudo. Point DNS at it before the first run so Caddy can get certificates.
set -euo pipefail

TARGET="${1:?usage: tools/deploy.sh user@server [DIR]}"
DIR="${2:-/srv/goober/host}"
HERE="$(cd "$(dirname "$0")" && pwd)"
HOST_SRC="$(dirname "$HERE")"

echo "── building the server binary"
(cd "$HOST_SRC" && { [ -f build/server/build.ninja ] || cmake --preset server >/dev/null; } && cmake --build --preset server)
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
strip -o "$STAGE/goober-host" "$HOST_SRC/build/server/packages/host/goober-host"
cp "$HERE/setup.sh" "$HERE/server-bootstrap.sh" "$STAGE/"

echo "── copying to $TARGET"
# A fresh private directory (mktemp: 0700, owned by the ssh user), so nobody else on the
# server can swap what root is about to run.
REMOTE="$(ssh "$TARGET" 'mktemp -d /tmp/goober-deploy.XXXXXXXX')"
[[ "$REMOTE" =~ ^/tmp/goober-deploy\.[A-Za-z0-9]+$ ]] || { echo "unexpected staging directory: $REMOTE" >&2; exit 1; }
scp -q "$STAGE/goober-host" "$STAGE/setup.sh" "$STAGE/server-bootstrap.sh" "$TARGET:$REMOTE/"

echo "── installing on $TARGET"
ssh -t "$TARGET" "sudo bash $REMOTE/server-bootstrap.sh $(printf %q "$DIR"); rm -rf $REMOTE"

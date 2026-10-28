#!/usr/bin/env bash
# Deploys goober to a server over ssh: builds the server binary here, copies it, and runs
# tools/server-bootstrap.sh there (Caddy, a goober user, setup, systemd units, backups).
#
#   tools/deploy.sh [user@]server [DIR] [--user LOGIN] [--run-as NAME] [--with-rust]
#
# --user LOGIN is who ssh logs in as on the server (the same as LOGIN@server; without
# either, ssh uses your local name). That account needs sudo.
# --run-as NAME is the system user the host runs as on the server (default goober). Its
# home is /srv/NAME, and DIR must be under it (default /srv/NAME/host).
# ssh connects once and reuses the connection, so a password is asked for once, not per step.
# --with-rust also installs a Rust toolchain (rustup, for the host's user, with the
# wasm32-unknown-unknown target) and the vendored crates (tools/vendor.sh), so agents can
# build Rust boxes from source projects on the server (521.Source.md).
# DIR is the host directory on the server (default /srv/NAME/host). The first run walks
# through setup interactively on the server; later runs replace the binary and restart.
# The server needs Ubuntu 24.04 or newer, or Debian 13 or newer (glibc 2.38), x86_64, and
# sudo. Point DNS at it before the first run so Caddy can get certificates.
set -euo pipefail

USAGE="usage: tools/deploy.sh [user@]server [DIR] [--user LOGIN] [--run-as NAME] [--with-rust]"
TARGET="${1:?$USAGE}"
DIR=""
LOGIN=""
RUN_AS="goober"
WITH_RUST=""
shift
while (($#)); do
  case "$1" in
    --with-rust) WITH_RUST=--with-rust ;;
    --user) LOGIN="${2:?$USAGE}"; shift ;;
    --user=*) LOGIN="${1#--user=}" ;;
    --run-as) RUN_AS="${2:?$USAGE}"; shift ;;
    --run-as=*) RUN_AS="${1#--run-as=}" ;;
    -*) echo "unknown option $1; $USAGE" >&2; exit 2 ;;
    *) DIR="$1" ;;
  esac
  shift
done
if [[ -n "$LOGIN" ]]; then
  [[ "$LOGIN" =~ ^[A-Za-z0-9_][A-Za-z0-9._-]*$ ]] || { echo "--user must be a login name, like customer" >&2; exit 2; }
  [[ "$TARGET" != *@* || "${TARGET%%@*}" == "$LOGIN" ]] ||
    { echo "the target says ${TARGET%%@*}@ but --user says $LOGIN; give one" >&2; exit 2; }
  TARGET="$LOGIN@${TARGET#*@}"
fi
# Checked here and again on the server: a plain system user name, never root.
[[ "$RUN_AS" =~ ^[a-z_][a-z0-9_-]{0,31}$ && "$RUN_AS" != root ]] ||
  { echo "--run-as must be a lowercase user name other than root, like goober" >&2; exit 2; }
DIR="${DIR:-/srv/$RUN_AS/host}"
HERE="$(cd "$(dirname "$0")" && pwd)"
HOST_SRC="$(dirname "$HERE")"

echo "── building the server binary"
(cd "$HOST_SRC" && { [ -f build/server/build.ninja ] || cmake --preset server >/dev/null; } && cmake --build --preset server)
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
strip -o "$STAGE/goober-host" "$HOST_SRC/build/server/packages/host/goober-host"
cp "$HERE/setup.sh" "$HERE/server-bootstrap.sh" "$STAGE/"
FILES=("$STAGE/goober-host" "$STAGE/setup.sh" "$STAGE/server-bootstrap.sh")
if [[ -n "$WITH_RUST" ]]; then
  echo "── vendoring the crates boxes may use"
  "$HERE/vendor.sh" "$STAGE/vendor"
  tar -czf "$STAGE/vendor.tar.gz" -C "$STAGE" vendor
  FILES+=("$STAGE/vendor.tar.gz")
fi

# One ssh connection for every step, its socket in the private staging directory.
SSH_OPTS=(-o ControlMaster=auto -o "ControlPath=$STAGE/ssh-%C" -o ControlPersist=120)
trap 'ssh "${SSH_OPTS[@]}" -O exit "$TARGET" 2>/dev/null; rm -rf "$STAGE"' EXIT

echo "── copying to $TARGET"
# A fresh private directory (mktemp: 0700, owned by the ssh user), so nobody else on the
# server can swap what root is about to run.
REMOTE="$(ssh "${SSH_OPTS[@]}" "$TARGET" 'mktemp -d /tmp/goober-deploy.XXXXXXXX')"
[[ "$REMOTE" =~ ^/tmp/goober-deploy\.[A-Za-z0-9]+$ ]] || { echo "unexpected staging directory: $REMOTE" >&2; exit 1; }
scp -q "${SSH_OPTS[@]}" "${FILES[@]}" "$TARGET:$REMOTE/"

echo "── installing on $TARGET"
ssh -t "${SSH_OPTS[@]}" "$TARGET" "sudo bash $REMOTE/server-bootstrap.sh $(printf %q "$DIR") --user $RUN_AS $WITH_RUST; rm -rf $REMOTE"

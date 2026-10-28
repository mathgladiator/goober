#!/usr/bin/env bash
# Runs on the server, as root, from tools/deploy.sh. Installs or upgrades goober:
#   - checks the system (glibc 2.38+, systemd), installs Caddy, curl, python3, tzdata;
#   - creates the system user (deploy.sh --run-as NAME, default goober, home /srv/NAME) and DIR, owned by it;
#   - installs /usr/local/bin/goober-host;
#   - first time: runs setup.sh as that user (interactive), then installs the systemd units,
#     the nightly backup timer, and the Caddyfile;
#   - later times: restarts the host on the new binary.
set -euo pipefail

#   server-bootstrap.sh [DIR] [--user NAME] [--with-rust]   (deploy.sh passes --with-rust unless --without-rust)
DIR=""
RUN_AS="goober"
WITH_RUST=""
while (($#)); do
  case "$1" in
    --with-rust) WITH_RUST=--with-rust ;;
    --user) RUN_AS="${2:-}"; shift ;;
    -*) echo "✗ unknown option $1" >&2; exit 2 ;;
    *) DIR="$1" ;;
  esac
  shift
done
SRC="$(cd "$(dirname "$0")" && pwd)"
BIN=/usr/local/bin/goober-host

die() { echo "✗ $*" >&2; exit 1; }
[[ $EUID -eq 0 ]] || die "run as root"
[[ "$RUN_AS" =~ ^[a-z_][a-z0-9_-]{0,31}$ && "$RUN_AS" != root ]] ||
  die "--run-as must be a lowercase user name other than root, like goober"
HOME_DIR="/srv/$RUN_AS"
DIR="${DIR:-$HOME_DIR/host}"
# DIR lives under the user's home, so nothing outside it is ever given to that user.
[[ "$DIR" =~ ^/srv/[a-z0-9_-]+/[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*$ && "$DIR" == "$HOME_DIR"/* && "$DIR" != *..* ]] ||
  die "DIR must be a directory under $HOME_DIR, like $HOME_DIR/host"
# An existing account is used only if it is the one this script would have made: a system
# account, not root, with its home at /srv/NAME. A person's login is never handed the host.
if id "$RUN_AS" >/dev/null 2>&1; then
  IFS=: read -r _ _ EXISTING_UID _ _ EXISTING_HOME _ < <(getent passwd "$RUN_AS")
  [[ "$EXISTING_UID" -ne 0 && "$EXISTING_HOME" == "$HOME_DIR" ]] ||
    die "user $RUN_AS exists with home $EXISTING_HOME; choose another --run-as, or one whose home is $HOME_DIR"
fi
[[ "$(uname -m)" == x86_64 ]] || die "the binary is built for x86_64"
GLIBC="$(ldd --version | head -1 | grep -oE '[0-9]+\.[0-9]+$')"
[[ "$(printf '%s\n2.38\n' "$GLIBC" | sort -V | head -1)" == 2.38 ]] ||
  die "glibc $GLIBC is too old: use Ubuntu 24.04+ or Debian 13+"

echo "── packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# bubblewrap is the sandbox source projects build in (521.Source.md).
apt-get install -y -qq caddy curl python3 tzdata ca-certificates bubblewrap >/dev/null

echo "── user and directory"
id "$RUN_AS" >/dev/null 2>&1 || useradd --system --home-dir "$HOME_DIR" --create-home --shell /usr/sbin/nologin "$RUN_AS"
install -d -o "$RUN_AS" -g "$RUN_AS" -m 0750 "$HOME_DIR" "$DIR"

echo "── binary"
install -m 0755 "$SRC/goober-host" "$BIN.new"
mv "$BIN.new" "$BIN"
mkdir -p "$HOME_DIR/tools"
install -m 0755 "$SRC/setup.sh" "$HOME_DIR/tools/setup.sh"

if [[ "$WITH_RUST" == --with-rust ]]; then
  echo "── Rust for building boxes from source"
  CARGO_BIN="$HOME_DIR/.cargo/bin"
  if [[ ! -x "$CARGO_BIN/cargo" ]]; then
    sudo -u "$RUN_AS" -H sh -c 'curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --target wasm32-unknown-unknown' >/dev/null
  fi
  # Every deploy: the wasm target is there (a no-op when it is). The toolchain's version stays
  # where it is; `rustup update` as the user moves it.
  sudo -u "$RUN_AS" -H "$CARGO_BIN/rustup" target add wasm32-unknown-unknown >/dev/null 2>&1 || true
  echo "   $(sudo -u "$RUN_AS" -H "$CARGO_BIN/cargo" --version)"
  # The crates boxes may use, vendored fresh by the host's user (it has cargo and the network),
  # then copied by root into a root-owned directory the builds read but can't change. A failure
  # (no network) keeps the crates from the last deploy.
  install -m 0755 "$SRC/vendor.sh" "$HOME_DIR/tools/vendor.sh"
  USER_STAGE="$(sudo -u "$RUN_AS" mktemp -d -p "$HOME_DIR" vendor-stage.XXXXXXXX)"
  if sudo -u "$RUN_AS" -H env PATH="$CARGO_BIN:$PATH" bash "$HOME_DIR/tools/vendor.sh" "$USER_STAGE/vendor"; then
    STAGE_VENDOR="$(mktemp -d)"
    cp -R --no-dereference --preserve=mode,timestamps "$USER_STAGE/vendor/." "$STAGE_VENDOR/"
    chown -R root:root "$STAGE_VENDOR"
    chmod -R a+rX,go-w "$STAGE_VENDOR"
    rm -rf "$HOME_DIR/vendor"
    mv -T "$STAGE_VENDOR" "$HOME_DIR/vendor"
  else
    echo "   couldn't vendor the crates; keeping what was there"
  fi
  rm -rf "$USER_STAGE"
fi

if [[ -f "$DIR/host.json" ]]; then
  echo "── upgrading: checking the config with the new binary, then restarting"
  sudo -u "$RUN_AS" "$BIN" check --config "$DIR/host.json"
  systemctl restart goober-host
else
  echo "── first install: setup (answer Production)"
  # setup.sh runs from a directory the user can read, writing into DIR as that user.
  (cd "$HOME_DIR" && sudo -u "$RUN_AS" GOOBER_HOST_BIN="$BIN" GOOBER_USER="$RUN_AS" bash "$HOME_DIR/tools/setup.sh" "$DIR")
  [[ -f "$DIR/deploy/goober-host.service" ]] || die "setup didn't write production files; run it again and answer Production"
  install -m 0644 "$DIR/deploy/goober-host.service" "$DIR/deploy/goober-backup.service" "$DIR/deploy/goober-backup.timer" \
    /etc/systemd/system/
  [[ -f /etc/caddy/Caddyfile && ! -f /etc/caddy/Caddyfile.before-goober ]] && cp /etc/caddy/Caddyfile /etc/caddy/Caddyfile.before-goober
  install -m 0644 "$DIR/deploy/Caddyfile" /etc/caddy/Caddyfile
  systemctl daemon-reload
  systemctl enable --now goober-host goober-backup.timer
  systemctl reload caddy || systemctl restart caddy
fi

if [[ -d "$HOME_DIR/vendor" && -f "$DIR/host.json" ]]; then
  # Builds read the vendored crates, and find cargo in the user's home.
  if sudo -u "$RUN_AS" python3 -c '
import json, sys
p, home = sys.argv[1], sys.argv[2]
c = json.load(open(p))
src = c.setdefault("source", {})
if src.get("vendor") == home + "/vendor" and "cargo" in src:
    sys.exit(1)
src["vendor"] = home + "/vendor"
src.setdefault("cargo", home + "/.cargo/bin/cargo")
open(p, "w").write(json.dumps(c, indent=2) + "\n")
' "$DIR/host.json" "$HOME_DIR"; then
    echo "set source.vendor and source.cargo in host.json"
    systemctl restart goober-host
  fi
fi

echo "── health"
PORT="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["listen"].get("port", 8080))' "$DIR/host.json")"
for _ in $(seq 1 50); do curl -fsS "http://127.0.0.1:$PORT/goober/health" >/dev/null 2>&1 && break; sleep 0.2; done
curl -fsS "http://127.0.0.1:$PORT/goober/health" && echo
systemctl --no-pager --lines=0 status goober-host | head -3 || true

cat <<EOF

Done. Next, on this server:
  sudo -u $RUN_AS $BIN token create --config $DIR/host.json --name claude-code --claude
mints an operator token; use it from your laptop as GOOBER_URL=https://<domain>
GOOBER_TOKEN=<token> host-src/tools/goober.py ..., or with the printed claude command.
Logs: journalctl -u goober-host -f, and $DIR/storage/logs/. Backups: $DIR/backups/.
EOF

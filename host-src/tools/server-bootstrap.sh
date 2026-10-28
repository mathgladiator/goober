#!/usr/bin/env bash
# Runs on the server, as root, from tools/deploy.sh. Installs or upgrades goober:
#   - checks the system (glibc 2.38+, systemd), installs Caddy, curl, python3, tzdata;
#   - creates the goober system user and DIR, owned by it;
#   - installs /usr/local/bin/goober-host;
#   - first time: runs setup.sh as goober (interactive), then installs the systemd units,
#     the nightly backup timer, and the Caddyfile;
#   - later times: restarts the host on the new binary.
set -euo pipefail

DIR="${1:-/srv/goober/host}"
WITH_RUST="${2:-}"
SRC="$(cd "$(dirname "$0")" && pwd)"
BIN=/usr/local/bin/goober-host

die() { echo "✗ $*" >&2; exit 1; }
[[ $EUID -eq 0 ]] || die "run as root"
# DIR lives under goober's home, so nothing outside it is ever given to the goober user.
[[ "$DIR" =~ ^/srv/goober/[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*$ && "$DIR" != *..* ]] ||
  die "DIR must be a directory under /srv/goober, like /srv/goober/host"
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
id goober >/dev/null 2>&1 || useradd --system --home-dir /srv/goober --create-home --shell /usr/sbin/nologin goober
install -d -o goober -g goober -m 0750 /srv/goober "$DIR"

echo "── binary"
install -m 0755 "$SRC/goober-host" "$BIN.new"
mv "$BIN.new" "$BIN"
mkdir -p /srv/goober/tools
install -m 0755 "$SRC/setup.sh" /srv/goober/tools/setup.sh

if [[ "$WITH_RUST" == --with-rust ]]; then
  echo "── Rust for building boxes from source"
  if [[ ! -x /srv/goober/.cargo/bin/cargo ]]; then
    sudo -u goober -H sh -c 'curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --target wasm32-unknown-unknown' >/dev/null
  fi
  if [[ -f "$SRC/vendor.tar.gz" ]]; then
    rm -rf /srv/goober/vendor.new
    mkdir -p /srv/goober/vendor.new
    tar -xzf "$SRC/vendor.tar.gz" -C /srv/goober/vendor.new --strip-components=1 --no-same-owner
    rm -rf /srv/goober/vendor
    mv /srv/goober/vendor.new /srv/goober/vendor
    chown -R goober:goober /srv/goober/vendor
  fi
fi

if [[ -f "$DIR/host.json" ]]; then
  echo "── upgrading: checking the config with the new binary, then restarting"
  sudo -u goober "$BIN" check --config "$DIR/host.json"
  systemctl restart goober-host
else
  echo "── first install: setup (answer Production)"
  # setup.sh runs from a directory goober can read, writing into DIR as goober.
  (cd /srv/goober && sudo -u goober GOOBER_HOST_BIN="$BIN" GOOBER_USER=goober bash /srv/goober/tools/setup.sh "$DIR")
  [[ -f "$DIR/deploy/goober-host.service" ]] || die "setup didn't write production files; run it again and answer Production"
  install -m 0644 "$DIR/deploy/goober-host.service" "$DIR/deploy/goober-backup.service" "$DIR/deploy/goober-backup.timer" \
    /etc/systemd/system/
  [[ -f /etc/caddy/Caddyfile && ! -f /etc/caddy/Caddyfile.before-goober ]] && cp /etc/caddy/Caddyfile /etc/caddy/Caddyfile.before-goober
  install -m 0644 "$DIR/deploy/Caddyfile" /etc/caddy/Caddyfile
  systemctl daemon-reload
  systemctl enable --now goober-host goober-backup.timer
  systemctl reload caddy || systemctl restart caddy
fi

if [[ -d /srv/goober/vendor && -f "$DIR/host.json" ]]; then
  # Builds read the vendored crates, and find cargo in the goober user's home.
  if sudo -u goober python3 -c '
import json, sys
p = sys.argv[1]
c = json.load(open(p))
src = c.setdefault("source", {})
if src.get("vendor") == "/srv/goober/vendor" and "cargo" in src:
    sys.exit(1)
src["vendor"] = "/srv/goober/vendor"
src.setdefault("cargo", "/srv/goober/.cargo/bin/cargo")
open(p, "w").write(json.dumps(c, indent=2) + "\n")
' "$DIR/host.json"; then
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
  sudo -u goober $BIN token create --config $DIR/host.json --name claude-code --claude
mints an operator token; use it from your laptop as GOOBER_URL=https://<domain>
GOOBER_TOKEN=<token> host-src/tools/goober.py ..., or with the printed claude command.
Logs: journalctl -u goober-host -f, and $DIR/storage/logs/. Backups: $DIR/backups/.
EOF

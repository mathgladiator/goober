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
WITHOUT_MAIL=""
while (($#)); do
  case "$1" in
    --with-rust) WITH_RUST=--with-rust ;;
    --without-mail) WITHOUT_MAIL=1 ;;
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
# iproute2 and openssl: Stalwart's provisioning namespace, and its first certificate.
apt-get install -y -qq caddy curl python3 tzdata ca-certificates bubblewrap iproute2 openssl >/dev/null
# A C toolchain: Rust's build scripts and proc macros (serde's derive) link for this machine.
[[ "$WITH_RUST" == --with-rust ]] && apt-get install -y -qq build-essential >/dev/null

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

PORT="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["listen"].get("port", 8080))' "$DIR/host.json")"
MAIL_DNS=""
if [[ -z "$WITHOUT_MAIL" && -f "$DIR/host.json" ]]; then
  echo "── mail in: Stalwart on port 25, handing every message to goober (024.MailIn.md)"
  MAIL_HOME=/srv/stalwart
  SW=/usr/local/lib/goober/stalwart.sh
  install -d -m 0755 /usr/local/lib/goober
  install -m 0755 "$SRC/stalwart.sh" "$SW"
  if id stalwart >/dev/null 2>&1; then
    IFS=: read -r _ _ SW_UID _ _ SW_HOME _ < <(getent passwd stalwart)
    [[ "$SW_UID" -ne 0 && "$SW_HOME" == "$MAIL_HOME" ]] || die "user stalwart exists with home $SW_HOME; expected $MAIL_HOME"
  else
    useradd --system --home-dir "$MAIL_HOME" --create-home --shell /usr/sbin/nologin stalwart
  fi
  install -d -o stalwart -g stalwart -m 0750 "$MAIL_HOME"
  # The binary is root's (pinned version and checksum in stalwart.sh); its data is Stalwart's.
  bash "$SW" install "$MAIL_HOME"
  chown -R root:root "$MAIL_HOME/bin"
  # One token, two copies: Stalwart sends it, goober checks it. Made once.
  HOST_TOKEN="$DIR/mail-hook.token"
  if [[ ! -s "$HOST_TOKEN" ]]; then
    (umask 077 && head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$HOST_TOKEN")
  fi
  chown "$RUN_AS:$RUN_AS" "$HOST_TOKEN"
  chmod 0600 "$HOST_TOKEN"
  install -o stalwart -g stalwart -m 0600 "$HOST_TOKEN" "$MAIL_HOME/hook.token"
  # STARTTLS with Caddy's certificate for the platform domain, once Caddy has one; until then
  # a short-lived self-signed one. The daily timer below copies renewals and reloads.
  PLATFORM="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("platform-domain", ""))' "$DIR/host.json")"
  install -d -o stalwart -g stalwart -m 0700 "$MAIL_HOME/tls"
  cat > /usr/local/lib/goober/stalwart-tls.sh <<'TLS'
#!/usr/bin/env bash
# Copies Caddy's certificate for the platform domain to Stalwart, and reloads it when it
# changed. Run as root (Caddy's files are its own); installed by server-bootstrap.sh.
set -euo pipefail
NAME="${1:?platform domain}"
DEST=/srv/stalwart/tls
CRT="$(find /var/lib/caddy/.local/share/caddy/certificates -path "*/$NAME/$NAME.crt" 2>/dev/null | head -n 1)"
if [[ -z "$CRT" || ! -f "${CRT%.crt}.key" ]]; then
  if [[ ! -s "$DEST/cert.pem" ]]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 30 -subj "/CN=$NAME" \
      -keyout "$DEST/key.pem" -out "$DEST/cert.pem" >/dev/null 2>&1
    chown stalwart:stalwart "$DEST/key.pem" "$DEST/cert.pem"
    chmod 0600 "$DEST/key.pem" "$DEST/cert.pem"
  fi
  exit 0
fi
if cmp -s "$CRT" "$DEST/cert.pem" && cmp -s "${CRT%.crt}.key" "$DEST/key.pem"; then exit 0; fi
install -o stalwart -g stalwart -m 0600 "$CRT" "$DEST/cert.pem"
install -o stalwart -g stalwart -m 0600 "${CRT%.crt}.key" "$DEST/key.pem"
if systemctl is-active --quiet stalwart; then
  runuser -u stalwart -- /usr/local/lib/goober/stalwart.sh reload-tls /srv/stalwart
fi
TLS
  chmod 0755 /usr/local/lib/goober/stalwart-tls.sh
  /usr/local/lib/goober/stalwart-tls.sh "${PLATFORM:-localhost}"
  # Relaying only for the host's domains, read from the config on every deploy.
  RELAY="$(python3 -c '
import json, re, sys
c = json.load(open(sys.argv[1]))
print(",".join(d for d in sorted(c.get("domains", {})) if re.fullmatch(r"[a-z0-9-]+(\.[a-z0-9-]+)+", d) and not re.fullmatch(r"[0-9.]+", d)))
' "$DIR/host.json")"
  # configure runs with Stalwart stopped, in a private network namespace root makes: its
  # temporary admin listener (on every interface, Stalwart's choice) never meets a network.
  systemctl stop stalwart 2>/dev/null || true
  unshare --net -- sh -c 'ip link set lo up && exec runuser -u stalwart -- env STALWART_PRIVATE_NETNS=1 "$@"' sh \
    bash "$SW" configure "$MAIL_HOME" --smtp 0.0.0.0:25 --admin 127.0.0.1:8025 \
    --hook-url "http://127.0.0.1:$PORT/internal/routes/mta/stalwart" --hook-token-file "$MAIL_HOME/hook.token" \
    --max-bytes 26214400 --hostname "${PLATFORM:-localhost}" ${RELAY:+--relay-domains "$RELAY"} \
    --cert "$MAIL_HOME/tls/cert.pem" --key "$MAIL_HOME/tls/key.pem"
  bash "$SW" systemd "$MAIL_HOME" stalwart > /etc/systemd/system/stalwart.service
  cat > /etc/systemd/system/goober-stalwart-tls.service <<UNIT
[Unit]
Description=Copy Caddy's certificate to Stalwart
[Service]
Type=oneshot
ExecStart=/usr/local/lib/goober/stalwart-tls.sh ${PLATFORM:-localhost}
UNIT
  cat > /etc/systemd/system/goober-stalwart-tls.timer <<UNIT
[Unit]
Description=Copy Caddy's certificate to Stalwart, daily
[Timer]
OnCalendar=daily
RandomizedDelaySec=1h
Persistent=true
[Install]
WantedBy=timers.target
UNIT
  systemctl daemon-reload
  systemctl enable --now stalwart goober-stalwart-tls.timer >/dev/null 2>&1
  systemctl restart stalwart
  # goober takes the hook once its config names the token.
  if sudo -u "$RUN_AS" python3 -c '
import json, sys
p, token = sys.argv[1], sys.argv[2]
c = json.load(open(p))
inbound = c.setdefault("mail", {}).setdefault("inbound", {})
if inbound.get("token-file") == token:
    sys.exit(1)
inbound["token-file"] = token
open(p, "w").write(json.dumps(c, indent=2) + "\n")
' "$DIR/host.json" "$HOST_TOKEN"; then
    echo "   set mail.inbound.token-file in host.json"
    systemctl restart goober-host
  fi
  systemctl --no-pager --lines=0 status stalwart | head -3 || true
  MAIL_DNS="$(python3 -c '
import json, re, sys
c = json.load(open(sys.argv[1]))
mx = c.get("platform-domain", "")
for d in sorted(c.get("domains", {})):
    if re.fullmatch(r"[a-z0-9-]+(\.[a-z0-9-]+)+", d) and not re.fullmatch(r"[0-9.]+", d):
        print(f"  {d}.  MX  10 {mx}.")
' "$DIR/host.json")"
fi

echo "── health"
for _ in $(seq 1 50); do curl -fsS "http://127.0.0.1:$PORT/goober/health" >/dev/null 2>&1 && break; sleep 0.2; done
curl -fsS "http://127.0.0.1:$PORT/goober/health" && echo
systemctl --no-pager --lines=0 status goober-host | head -3 || true

echo "── checking everything this host needs"
FAILED=()
check() {
  local what="$1"; shift
  if "$@" >/tmp/goober-check.$$ 2>&1; then
    echo "  ✓ $what"
  else
    echo "  ✗ $what"
    sed 's/^/      /' /tmp/goober-check.$$ | tail -n 15
    FAILED+=("$what")
  fi
  rm -f /tmp/goober-check.$$
}
check "goober-host is running" systemctl is-active --quiet goober-host
check "goober-host answers its health check" curl -fsS "http://127.0.0.1:$PORT/goober/health"
check "Caddy is running" systemctl is-active --quiet caddy
check "the nightly backup timer is on" systemctl is-active --quiet goober-backup.timer
if [[ "$WITH_RUST" == --with-rust ]]; then
  check "a C toolchain (cc) for crates' build scripts" sh -c 'command -v cc && cc --version | head -n 1'
  check "bubblewrap, the build sandbox" sh -c 'command -v bwrap && bwrap --version'
  wasm_target() { sudo -u "$RUN_AS" -H "$HOME_DIR/.cargo/bin/rustup" target list --installed | grep -x wasm32-unknown-unknown; }
  check "cargo with the wasm32 target, for $RUN_AS" wasm_target
  # The whole chain, as an agent's build meets it: the host's builder, its sandbox, offline.
  check "a Rust box builds the way source_deploy builds it" sudo -u "$RUN_AS" -H "$BIN" toolchain --config "$DIR/host.json"
fi
if [[ -z "$WITHOUT_MAIL" ]]; then
  check "Stalwart is running" systemctl is-active --quiet stalwart
  check "the certificate timer is on" systemctl is-active --quiet goober-stalwart-tls.timer
  hook_answers() {
    local code
    code="$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Authorization: Bearer $(cat "$HOST_TOKEN")" \
      -H 'content-type: application/json' -d '{"context":{"stage":"ehlo"}}' \
      "http://127.0.0.1:$PORT/internal/routes/mta/stalwart")"
    echo "HTTP $code"
    [[ "$code" == 200 ]]
  }
  check "the host takes Stalwart's hook" hook_answers
  # SMTP on port 25, and Stalwart asking the host: an address nobody serves is refused at RCPT.
  check "SMTP on port 25 refuses an address no box claims" python3 -I -c '
import smtplib
s = smtplib.SMTP("127.0.0.1", 25, local_hostname="deploy-check.invalid", timeout=60)
s.ehlo()
s.mail("")  # a null sender: nothing about it to look up
code, text = s.rcpt("nobody@deploy-check.invalid")
s.quit()
print(code, text.decode())
raise SystemExit(0 if code == 550 else 1)
'
fi

cat <<EOF

Done. Next, on this server:
  sudo -u $RUN_AS $BIN token create --config $DIR/host.json --name claude-code --claude
mints an operator token; use it from your laptop as GOOBER_URL=https://<domain>
GOOBER_TOKEN=<token> host-src/tools/goober.py ..., or with the printed claude command.
Logs: journalctl -u goober-host -f, and $DIR/storage/logs/. Backups: $DIR/backups/.
EOF
if [[ -n "$MAIL_DNS" ]]; then
  cat <<EOF

Mail in: Stalwart listens on port 25 (journalctl -u stalwart -f). Add these DNS records, and
open TCP port 25 inbound in your provider's firewall (many block it until asked):
$MAIL_DNS
Then declare routes.mail in a box (box_guide, "Receiving mail").
EOF
fi

if ((${#FAILED[@]})); then
  echo
  echo "✗ ${#FAILED[@]} check(s) failed: ${FAILED[*]}"
  echo "  Everything else is installed; fix these (the output above says why) and deploy again."
  exit 1
fi
echo
echo "✓ every check passed"

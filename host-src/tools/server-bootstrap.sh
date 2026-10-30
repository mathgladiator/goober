#!/usr/bin/env bash
# Runs on the server, as root, from tools/deploy.sh. Installs or upgrades goober:
#   - checks the system (glibc 2.38+, systemd), installs Caddy, curl, python3, tzdata;
#   - creates the system user (deploy.sh --run-as NAME, default goober, home /srv/NAME) and DIR, owned by it;
#   - installs /usr/local/bin/goober-host;
#   - first time: runs setup.sh as that user (interactive), then installs the systemd units,
#     the nightly backup timer, and the Caddyfile;
#   - later times: restarts the host on the new binary.
set -euo pipefail

#   server-bootstrap.sh [DIR] [--user NAME] [--with-rust] [--public DOMAIN] [--build COMMIT]
#   (deploy.sh passes --with-rust unless --without-rust, and the domain and build it deployed)
DIR=""
PUBLIC=""
BUILD=""
RUN_AS="goober"
WITH_RUST=""
WITHOUT_MAIL=""
while (($#)); do
  case "$1" in
    --with-rust) WITH_RUST=--with-rust ;;
    --without-mail) WITHOUT_MAIL=1 ;;
    --user) RUN_AS="${2:-}"; shift ;;
    --public) PUBLIC="${2:-}"; shift ;;
    --build) BUILD="${2:-}"; shift ;;
    -*) echo "✗ unknown option $1" >&2; exit 2 ;;
    *) DIR="$1" ;;
  esac
  shift
done
SRC="$(cd "$(dirname "$0")" && pwd)"
BIN=/usr/local/bin/goober-host

die() { echo "✗ $*" >&2; exit 1; }
[[ -z "$PUBLIC" || "$PUBLIC" =~ ^[a-z0-9-]+(\.[a-z0-9-]+)+$ ]] || die "--public is a domain, like kcmeatup.com"
[[ -z "$BUILD" || "$BUILD" =~ ^[0-9a-f]{7,40}$ ]] || die "--build is a commit, like 4f575dadb5d7"
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
# bubblewrap is the sandbox source projects build in (521.Source.md).
# iproute2 and openssl: Stalwart's provisioning namespace, and its first certificate.
PACKAGES=(caddy curl python3 tzdata ca-certificates bubblewrap iproute2 openssl)
# A C toolchain: Rust's build scripts and proc macros (serde's derive) link for this machine.
[[ "$WITH_RUST" == --with-rust ]] && PACKAGES+=(build-essential)
# The installed version, or "" when the package isn't installed.
installed() {
  local status version
  read -r status version < <(dpkg-query -W -f='${db:Status-Abbrev} ${Version}\n' "$1" 2>/dev/null || true)
  [[ "$status" == ii ]] && echo "$version" || true
}
declare -A BEFORE
for p in "${PACKAGES[@]}"; do BEFORE[$p]="$(installed "$p")"; done
echo "   apt-get update"
apt-get update -qq
# install also upgrades each one that has a newer version; what changed is listed below.
echo "   apt-get install ${PACKAGES[*]}"
apt-get install -y -qq "${PACKAGES[@]}" >/dev/null
for p in "${PACKAGES[@]}"; do
  now="$(installed "$p")"
  if [[ -z "${BEFORE[$p]}" ]]; then
    printf '   + %-16s %s (installed)\n' "$p" "$now"
  elif [[ "${BEFORE[$p]}" != "$now" ]]; then
    printf '   ↑ %-16s %s → %s\n' "$p" "${BEFORE[$p]}" "$now"
  else
    printf '   ✓ %-16s %s\n' "$p" "$now"
  fi
done

echo "── user and directory"
id "$RUN_AS" >/dev/null 2>&1 || useradd --system --home-dir "$HOME_DIR" --create-home --shell /usr/sbin/nologin "$RUN_AS"
install -d -o "$RUN_AS" -g "$RUN_AS" -m 0750 "$HOME_DIR"
# Inside the home, the host's user owns everything, so root never makes, writes, chowns, or
# reads a path there itself: a symlink planted by that user would aim root anywhere. The user
# does it (as_user), and root takes what it needs from the user's output.
as_user() { sudo -u "$RUN_AS" -H "$@"; }
# Installs a file root holds into the home, written by the host's user: as_user_install MODE SRC DEST.
as_user_install() { as_user sh -c 'cat > "$2.new" && chmod "$1" "$2.new" && mv -f "$2.new" "$2"' sh "$1" "$3" < "$2"; }
# host.json as the host's user reads it.
host_json() { as_user cat "$DIR/host.json"; }
[[ -L "$DIR" ]] && die "$DIR is a symlink; the host directory must be a real directory"
as_user mkdir -p -m 0750 "$DIR"

echo "── binary"
# The build that was serving is kept as $BIN.prev, so a new one that won't start is rolled back.
if [[ -f "$BIN" ]]; then cp -p "$BIN" "$BIN.prev"; fi
install -m 0755 "$SRC/goober-host" "$BIN.new"
mv "$BIN.new" "$BIN"
rollback() {
  [[ -f "$BIN.prev" ]] || { echo "   no previous build to roll back to"; return 1; }
  echo "── rolling back to the previous build: $("$BIN.prev" --version 2>/dev/null)"
  cp -p "$BIN.prev" "$BIN"
  systemctl restart goober-host || true
}
as_user mkdir -p "$HOME_DIR/tools"
as_user_install 0755 "$SRC/setup.sh" "$HOME_DIR/tools/setup.sh"

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
  as_user_install 0755 "$SRC/vendor.sh" "$HOME_DIR/tools/vendor.sh"
  USER_STAGE="$(sudo -u "$RUN_AS" mktemp -d -p "$HOME_DIR" vendor-stage.XXXXXXXX)"
  if sudo -u "$RUN_AS" -H env PATH="$CARGO_BIN:$PATH" bash "$HOME_DIR/tools/vendor.sh" "$USER_STAGE/vendor"; then
    STAGE_VENDOR="$(mktemp -d)"
    cp -R --no-dereference --preserve=mode,timestamps "$USER_STAGE/vendor/." "$STAGE_VENDOR/"
    chown -hR root:root "$STAGE_VENDOR"  # -h: a symlink in the crates is changed, never followed
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
  if ! sudo -u "$RUN_AS" "$BIN" check --config "$DIR/host.json"; then
    echo "✗ the new build refuses host.json (above); the running host is untouched"
    cp -p "$BIN.prev" "$BIN" 2>/dev/null || true
    exit 1
  fi
  systemctl restart goober-host
else
  echo "── first install: setup (answer Production)"
  # setup.sh runs from a directory the user can read, writing into DIR as that user.
  (cd "$HOME_DIR" && sudo -u "$RUN_AS" GOOBER_HOST_BIN="$BIN" GOOBER_USER="$RUN_AS" bash "$HOME_DIR/tools/setup.sh" "$DIR")
  [[ -f "$DIR/deploy/goober-host.service" ]] || die "setup didn't write production files; run it again and answer Production"
  # The units run as root's systemd, so what setup (the host's user) wrote is read as that user
  # and must be only what setup writes: these keys, run as the host's user, no Exec prefixes
  # (a "+" runs a command as root).
  UNIT_TMP="$(mktemp -d)"
  for unit in goober-host.service goober-backup.service goober-backup.timer; do
    as_user cat "$DIR/deploy/$unit" > "$UNIT_TMP/$unit"
    python3 -I - "$UNIT_TMP/$unit" "$RUN_AS" <<'PY' || die "$DIR/deploy/$unit isn't the unit setup writes; run setup again"
import re, sys
path, user = sys.argv[1:3]
allowed = {"Description", "After", "Wants", "User", "WorkingDirectory", "ExecStart", "ExecReload", "Restart",
           "RestartSec", "NoNewPrivileges", "PrivateTmp", "ProtectSystem", "ReadWritePaths", "LimitNOFILE",
           "WantedBy", "Type", "OnCalendar", "Persistent"}
users = 0
for line in open(path):
    line = line.strip()
    if not line or line.startswith("#") or re.fullmatch(r"\[(Unit|Service|Install|Timer)\]", line):
        continue
    key, _, value = line.partition("=")
    if key not in allowed:
        sys.exit("key " + key)
    if key == "User":
        users += 1
        if value != user:
            sys.exit("User=" + value)
    if key.startswith("Exec") and value[:1] in "+!@-:":
        sys.exit("prefixed " + key)
sys.exit(0 if users == 1 or path.endswith(".timer") else "User")
PY
  done
  install -m 0644 "$UNIT_TMP/goober-host.service" "$UNIT_TMP/goober-backup.service" "$UNIT_TMP/goober-backup.timer" \
    /etc/systemd/system/
  [[ -f /etc/caddy/Caddyfile && ! -f /etc/caddy/Caddyfile.before-goober ]] && cp /etc/caddy/Caddyfile /etc/caddy/Caddyfile.before-goober
  as_user cat "$DIR/deploy/Caddyfile" > "$UNIT_TMP/Caddyfile"
  install -m 0644 "$UNIT_TMP/Caddyfile" /etc/caddy/Caddyfile
  rm -rf "$UNIT_TMP"
  systemctl daemon-reload
  systemctl enable --now goober-host goober-backup.timer
  systemctl reload caddy || systemctl restart caddy
fi

# Caddyfiles written before 2026-10-30 proxy /internal/* (the mail hook's path) from outside.
# The host refuses it there too; Caddy now answers 404 first.
if [[ -f /etc/caddy/Caddyfile ]] && grep -q "health_uri /goober/health" /etc/caddy/Caddyfile && ! grep -q "respond /internal/\* 404" /etc/caddy/Caddyfile; then
  cp -p /etc/caddy/Caddyfile /etc/caddy/Caddyfile.before-internal
  sed -i 's|^  encode zstd gzip$|  encode zstd gzip\n  respond /internal/* 404|' /etc/caddy/Caddyfile
  if caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile >/dev/null 2>&1; then
    systemctl reload caddy
  else
    cp -p /etc/caddy/Caddyfile.before-internal /etc/caddy/Caddyfile  # put back what worked
    echo "!! couldn't add 'respond /internal/* 404' to /etc/caddy/Caddyfile; it's unchanged"
  fi
fi

# No multipath TCP (2026-10-30). The host listens on plain TCP, but Caddy is built with Go 1.24+,
# whose listeners take MPTCP whenever the kernel offers it, and the kernel's MPTCP code has had
# remotely reachable flaws (CVE-2026-98432, CVE-2026-80587). Off in the kernel, and off in Caddy
# should the kernel setting ever be undone. On every deploy, so existing servers get it too.
echo "── multipath TCP off"
if [[ -e /proc/sys/net/mptcp/enabled ]]; then
  echo "net.mptcp.enabled = 0" > /etc/sysctl.d/90-goober-no-mptcp.conf
  # A container may not let us write the kernel setting: say so, and carry on to the checks.
  sysctl -q -w net.mptcp.enabled=0 || echo "!! couldn't set net.mptcp.enabled=0 (a container?); Caddy's own setting still applies"
fi
mkdir -p /etc/systemd/system/caddy.service.d
DROPIN=/etc/systemd/system/caddy.service.d/goober-no-mptcp.conf
WANT=$'[Service]\nEnvironment=GODEBUG=multipathtcp=0\n'
if [[ "$(cat "$DROPIN" 2>/dev/null)"$'\n' != "$WANT" ]]; then
  printf '%s' "$WANT" > "$DROPIN"
  systemctl daemon-reload || echo "!! systemctl daemon-reload failed"
  systemctl restart caddy || echo "!! Caddy didn't restart with GODEBUG=multipathtcp=0; the checks below will say more"
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

PORT="$(host_json | python3 -I -c 'import json,sys; print(int(json.load(sys.stdin)["listen"].get("port", 8080)))')"
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
  # DIR is the host user's: root never writes, chowns, or reads through a path in it (a symlink
  # there would aim root at any file). The host's user makes its copy; root reads it as that user.
  HOST_TOKEN="$DIR/mail-hook.token"
  sudo -u "$RUN_AS" sh -c 'f="$1"; [ -s "$f" ] && [ ! -L "$f" ] || { rm -f "$f"; umask 077; head -c 32 /dev/urandom | od -An -tx1 | tr -d " \n" > "$f"; }; chmod 0600 "$f"' sh "$HOST_TOKEN"
  TOKEN_VALUE="$(sudo -u "$RUN_AS" cat "$HOST_TOKEN")"
  [[ "$TOKEN_VALUE" =~ ^[0-9a-f]{64}$ ]] || die "$HOST_TOKEN isn't the 64 hex digits the deploy wrote; remove it and deploy again"
  TOKEN_TMP="$(mktemp)"  # root's own, so no other user can aim the write elsewhere
  printf '%s' "$TOKEN_VALUE" > "$TOKEN_TMP"
  install -o stalwart -g stalwart -m 0600 "$TOKEN_TMP" "$MAIL_HOME/hook.token"
  rm -f "$TOKEN_TMP"
  # STARTTLS with Caddy's certificate for the platform domain, once Caddy has one; until then
  # a short-lived self-signed one. The daily timer below copies renewals and reloads.
  PLATFORM="$(host_json | python3 -I -c 'import json,sys; print(json.load(sys.stdin).get("platform-domain", ""))')"
  # A name root's certificate copy builds paths from: a DNS name, nothing else.
  [[ "$PLATFORM" =~ ^[a-z0-9-]+(\.[a-z0-9-]+)*$ ]] || PLATFORM=""
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
  RELAY="$(host_json | python3 -I -c '
import json, re, sys
c = json.load(sys.stdin)
print(",".join(d for d in sorted(c.get("domains", {})) if re.fullmatch(r"[a-z0-9-]+(\.[a-z0-9-]+)+", d) and not re.fullmatch(r"[0-9.]+", d)))
')"
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
  MAIL_DNS="$(host_json | python3 -I -c '
import json, re, sys
c = json.load(sys.stdin)
mx = c.get("platform-domain", "")
for d in sorted(c.get("domains", {})):
    if re.fullmatch(r"[a-z0-9-]+(\.[a-z0-9-]+)+", d) and not re.fullmatch(r"[0-9.]+", d):
        print(f"  {d}.  MX  10 {mx}.")
')"
fi

# Caddyfiles written before 2026-10-29 answer an empty 503 the moment the host restarts or is
# down, which some MCP clients cache as "sign-in provider unavailable". Add a retry window and
# a retryable answer to the Caddyfile goober wrote, if it hasn't them; Caddy checks it first.
if [[ -f /etc/caddy/Caddyfile ]] && grep -q "health_uri /goober/health" /etc/caddy/Caddyfile && ! grep -q "lb_try_duration" /etc/caddy/Caddyfile; then
  echo "── Caddy: a retry window for restarts, and a retryable 503"
  NEW_CADDY="$(mktemp)"
  python3 - /etc/caddy/Caddyfile "$NEW_CADDY" <<'PY'
import sys
lines = open(sys.argv[1]).read().split("\n")
out, i, done = [], 0, False
while i < len(lines):
    line = lines[i]
    out.append(line)
    if not done and line.strip() == "health_uri /goober/health":
        pad = line[: len(line) - len(line.lstrip())]
        out += [pad + "lb_try_duration 15s", pad + "lb_try_interval 250ms"]
        # The end of the reverse_proxy block, then the site's own handle_errors.
        i += 1
        while i < len(lines) and lines[i].strip() != "}":
            out.append(lines[i]); i += 1
        out.append(lines[i])
        site = pad[:-2] if len(pad) >= 2 else ""
        out += [site + "handle_errors {",
                site + "  @down expression `{err.status_code} in [502, 503, 504]`",
                site + "  handle @down {",
                site + "    header Retry-After 15",
                site + "    header Cache-Control no-store",
                site + "    header Content-Type application/json",
                site + '    respond `{"error":"temporarily_unavailable","error_description":"the host is restarting; retry shortly"}` 503',
                site + "  }",
                site + "}"]
        done = True
    i += 1
open(sys.argv[2], "w").write("\n".join(out))
PY
  if caddy validate --config "$NEW_CADDY" --adapter caddyfile >/tmp/goober-caddy.$$ 2>&1; then
    cp -p /etc/caddy/Caddyfile "/etc/caddy/Caddyfile.before-retry"
    install -m 0644 "$NEW_CADDY" /etc/caddy/Caddyfile
    systemctl reload caddy || systemctl restart caddy
    echo "   added (the old one is /etc/caddy/Caddyfile.before-retry)"
  else
    echo "   Caddy refused the change, so the Caddyfile is as it was:"; sed 's/^/     /' /tmp/goober-caddy.$$ | tail -n 8
  fi
  rm -f "$NEW_CADDY" /tmp/goober-caddy.$$
fi

echo "── ready"
# The gate: the new build must say it's ready (/goober/ready: storage, identity, boxes, the
# queue) within a minute. If it doesn't, its log says why, and the previous build comes back.
ready() { curl -fsS -m 5 "http://127.0.0.1:$PORT/goober/ready" >/tmp/goober-ready.$$ 2>&1; }
up=""
for _ in $(seq 1 120); do if ready; then up=1; break; fi; sleep 0.5; done
if [[ -z "$up" ]]; then
  echo "✗ the new build isn't ready after 60 s. Its status and last log lines:"
  cat /tmp/goober-ready.$$ 2>/dev/null | sed 's/^/   /'; echo
  systemctl --no-pager --lines=0 status goober-host | sed 's/^/   /' | head -8 || true
  journalctl -u goober-host -n 60 --no-pager | sed 's/^/   /'
  rm -f /tmp/goober-ready.$$
  if rollback; then
    for _ in $(seq 1 60); do curl -fsS -m 5 "http://127.0.0.1:$PORT/goober/health" >/dev/null 2>&1 && break; sleep 0.5; done
    if curl -fsS -m 5 "http://127.0.0.1:$PORT/goober/health"; then echo; echo "   the previous build is serving again"; else echo "   ✗ the previous build isn't answering either"; fi
  fi
  exit 1
fi
cat /tmp/goober-ready.$$; echo; rm -f /tmp/goober-ready.$$
systemctl --no-pager --lines=0 status goober-host | head -3 || true

echo "── versions"
printf '   %-14s %s\n' "goober-host" "$("$BIN" --version | sed 's/^goober-host //')"
printf '   %-14s %s\n' "caddy" "$(caddy version 2>/dev/null | cut -d' ' -f1)"
if [[ "$WITH_RUST" == --with-rust && -x "$HOME_DIR/.cargo/bin/rustc" ]]; then
  printf '   %-14s %s\n' "rustc" "$(sudo -u "$RUN_AS" -H "$HOME_DIR/.cargo/bin/rustc" --version | cut -d' ' -f2-)"
  printf '   %-14s %s\n' "cargo" "$(sudo -u "$RUN_AS" -H "$HOME_DIR/.cargo/bin/cargo" --version | cut -d' ' -f2-)"
  printf '   %-14s %s\n' "crates" "$(ls "$HOME_DIR/vendor" 2>/dev/null | tr '\n' ' ')"
fi
if [[ -z "$WITHOUT_MAIL" && -x /srv/stalwart/bin/stalwart ]]; then
  printf '   %-14s %s\n' "stalwart" "$(/srv/stalwart/bin/stalwart --version 2>/dev/null)"
fi

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
check "goober-host is ready (storage, identity, boxes)" curl -fsS "http://127.0.0.1:$PORT/goober/ready"
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
    code="$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Authorization: Bearer $TOKEN_VALUE" \
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

# The same checks people's browsers and agents depend on, from here through the public name,
# published for bots at https://$PUBLIC/goober/deploy (and summed up in /goober/ready).
if [[ -n "$PUBLIC" && -f "$SRC/readiness.py" ]]; then
  echo "── readiness, as people and agents reach https://$PUBLIC"
  as_user_install 0755 "$SRC/readiness.py" "$HOME_DIR/tools/readiness.py"
  EXTRA=()
  if [[ -f "$SRC/readiness.args" ]]; then
    while IFS= read -r line; do [[ -z "$line" || "$line" == \#* ]] || EXTRA+=($line); done <"$SRC/readiness.args"
  fi
  as_user mkdir -p -m 0750 "$DIR/storage/operations"
  if ! sudo -u "$RUN_AS" python3 -I "$HOME_DIR/tools/readiness.py" "https://$PUBLIC" ${BUILD:+--build "$BUILD"} "${EXTRA[@]}" \
      --json "$DIR/storage/operations/deploy-check.json"; then
    FAILED+=("readiness from https://$PUBLIC (see /goober/deploy)")
  fi
fi

# The operations audit (028.Panel.md § 8): not whether the host is up, but how it is run: the
# panel, mail, backups, operators, Caddy. It advises and never fails a deploy. A check named in
# host.json's "audit": {"quiet": [...]} is noted as quiet instead. The panel it can set up here.
echo "── operations audit"
AUDIT_ARGS=(--config "$DIR/host.json")
[[ -r /etc/caddy/Caddyfile ]] && AUDIT_ARGS+=(--caddyfile /etc/caddy/Caddyfile)
sudo -u "$RUN_AS" "$BIN" audit "${AUDIT_ARGS[@]}" || true
PANEL="$(sudo -u "$RUN_AS" "$BIN" audit "${AUDIT_ARGS[@]}" --json 2>/dev/null | python3 -I -c '
import json, sys
for f in json.load(sys.stdin)["findings"]:
    if f["id"] == "panel" and f["level"] == "suggest":
        print(f["data"]["domain"], f["data"]["dns"])
' 2>/dev/null || true)"

# Edits host.json as the host's user: "operations.domain" set, or "panel" added to audit.quiet.
edit_config() {
  sudo -u "$RUN_AS" python3 -I - "$DIR/host.json" "$1" "$2" <<'PY'
import json, os, sys
path, what, value = sys.argv[1:4]
cfg = json.load(open(path))
if what == "panel":
    cfg.setdefault("operations", {})["domain"] = value
else:
    quiet = cfg.setdefault("audit", {}).setdefault("quiet", [])
    if value not in quiet:
        quiet.append(value)
tmp = path + ".tmp"
with open(tmp, "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
os.chmod(tmp, os.stat(path).st_mode & 0o777)
os.replace(tmp, path)
PY
}

# The panel on its own domain: host.json, then the Caddyfile's site line, each checked, and
# both put back if either is refused.
panel_setup() {
  local d="$1"
  # host.json's copy is made and put back by the host's user: DIR is theirs (no root writes in it).
  sudo -u "$RUN_AS" cp --remove-destination "$DIR/host.json" "$DIR/host.json.before-panel" ||
    { echo "   couldn't copy host.json aside; nothing changed"; return 1; }
  cp -p /etc/caddy/Caddyfile /etc/caddy/Caddyfile.before-panel || { echo "   couldn't copy the Caddyfile aside; nothing changed"; return 1; }
  undo() {
    sudo -u "$RUN_AS" cp --remove-destination "$DIR/host.json.before-panel" "$DIR/host.json"
    cp -p /etc/caddy/Caddyfile.before-panel /etc/caddy/Caddyfile
    echo "   $1; host.json and the Caddyfile are as they were"
  }
  edit_config panel "$d" || { undo "couldn't edit host.json"; return 1; }
  sudo -u "$RUN_AS" "$BIN" check --config "$DIR/host.json" >/dev/null || { undo "the host refused the new host.json"; return 1; }
  # The site block that serves the site itself (ops.example.com joins example.com's), read the
  # way goober-host audit reads site lines: addresses without scheme or port.
  python3 -I - /etc/caddy/Caddyfile "$d" "${d#ops.}" <<'PY' || { undo "no site block in the Caddyfile serves ${d#ops.}"; return 1; }
import sys
path, d, site = sys.argv[1:4]
lines = open(path).read().split("\n")
for i, line in enumerate(lines):
    s = line.rstrip()
    if not (s.endswith("{") and len(s) > 1 and s[0] not in " \t#{("):
        continue
    names = []
    for a in s[:-1].replace(",", " ").split():
        a = a.split("://", 1)[-1]
        names.append(a.split(":", 1)[0].split("/", 1)[0].lower())
    if site in names:
        lines[i] = s[:-1].rstrip() + ", " + d + " {"
        break
else:
    sys.exit(1)
open(path, "w").write("\n".join(lines))
PY
  caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile >/dev/null 2>&1 || { undo "Caddy refused the new site line"; return 1; }
  # The checks above have run, so this restart must prove itself: ready within a minute, or
  # everything goes back and the deploy fails.
  systemctl restart goober-host || true
  local up=""
  for _ in $(seq 1 120); do curl -fsS -m 5 "http://127.0.0.1:$PORT/goober/ready" >/dev/null 2>&1 && { up=1; break; }; sleep 0.5; done
  if [[ -z "$up" ]]; then
    undo "the host wasn't ready with the panel set"
    systemctl restart goober-host || true
    FAILED+=("the panel's setup (host.json and the Caddyfile were put back)")
    return 1
  fi
  if ! systemctl reload caddy; then
    undo "Caddy wouldn't reload with the panel's site"
    systemctl restart goober-host || true
    systemctl reload caddy || true
    FAILED+=("the panel's setup (host.json and the Caddyfile were put back)")
    return 1
  fi
  # Caddy gets the certificate on the first request; a minute is plenty.
  local code=""
  for _ in $(seq 1 30); do
    code="$(curl -s -o /dev/null -m 10 -w '%{http_code}' "https://$d/" || true)"
    [[ "$code" =~ ^(200|302|303)$ ]] && break
    sleep 2
  done
  if [[ "$code" =~ ^(200|302|303)$ ]]; then
    echo "   ✓ the panel answers at https://$d/ (sign in with an operator address)"
  else
    echo "   ! set up, but https://$d/ answered HTTP ${code:-nothing} so far; Caddy may still be getting its certificate"
  fi
}

if [[ -n "$PANEL" ]]; then
  read -r PANEL_DOMAIN PANEL_DNS <<<"$PANEL"
  if [[ ! -t 0 ]]; then
    echo "   (deploy from a terminal to be offered the panel at https://$PANEL_DOMAIN/)"
  elif [[ "$PANEL_DNS" == ready ]]; then
    read -r -p "   Set up the panel at https://$PANEL_DOMAIN/ now? [y]es, [n]ot now, ne[v]er: " answer </dev/tty || answer=n
    case "$answer" in
      y|Y|yes) panel_setup "$PANEL_DOMAIN" || true ;;
      v|V|never) edit_config quiet panel && echo "   won't suggest it again (audit.quiet in host.json)" ;;
      *) echo "   not now; the next deploy asks again" ;;
    esac
  else
    read -r -p "   Add that DNS record and deploy again to set it up. Never suggest the panel? [v]/Enter: " answer </dev/tty || answer=""
    [[ "$answer" =~ ^(v|V|never)$ ]] && edit_config quiet panel && echo "   won't suggest it again (audit.quiet in host.json)"
  fi
fi

if ((${#FAILED[@]})); then
  echo
  echo "✗ ${#FAILED[@]} check(s) failed: ${FAILED[*]}"
  echo "  Everything else is installed; fix these (the output above says why) and deploy again."
  exit 1
fi
echo
echo "✓ every check passed"

#!/usr/bin/env bash
# Sets up a goober host in a directory, walking through the core decisions.
#
#   tools/setup.sh [--defaults] DIR
#
# Writes DIR/host.json and DIR/storage/, and for production DIR/deploy/ (a systemd
# unit and a Caddyfile). --defaults answers every question with the dev-box default
# and asks nothing, which is what `just run` uses on a fresh checkout.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
HOST_SRC="$(dirname "$HERE")"
BIN="${GOOBER_HOST_BIN:-$HOST_SRC/build/dev/packages/host/goober-host}"

DEFAULTS=0
if [[ "${1:-}" == "--defaults" ]]; then DEFAULTS=1; shift; fi
DIR="${1:-demo}"

# ---------------------------------------------------------------- presentation

if [[ -t 1 ]]; then
  B=$'\e[1m'; D=$'\e[2m'; G=$'\e[32m'; Y=$'\e[33m'; R=$'\e[31m'; C=$'\e[36m'; N=$'\e[0m'
else
  B=; D=; G=; Y=; R=; C=; N=
fi
STEP=0
step() { STEP=$((STEP + 1)); printf '\n%s%s── %d. %s %s\n' "$B" "$C" "$STEP" "$1" "$N"; }
say() { printf '%s\n' "$*"; }
note() { printf '%s%s%s\n' "$D" "$*" "$N"; }
warn() { printf '%s! %s%s\n' "$Y" "$*" "$N"; }
good() { printf '%s✓ %s%s\n' "$G" "$*" "$N"; }
die() { printf '%s✗ %s%s\n' "$R" "$*" "$N" >&2; exit 1; }

# ask VAR "Question" DEFAULT [VALIDATOR]: an editable default; re-asks until valid.
ask() {
  local var="$1" q="$2" def="$3" check="${4:-}" ans
  if [[ $DEFAULTS == 1 ]]; then printf -v "$var" '%s' "$def"; return; fi
  while true; do
    read -r -e -p "$B$q$N " -i "$def" ans || exit 1
    ans="${ans:-$def}"
    if [[ -z "$check" ]] || "$check" "$ans"; then printf -v "$var" '%s' "$ans"; return; fi
  done
}

# confirm VAR "Question" y|n
confirm() {
  local var="$1" q="$2" def="$3" ans hint
  if [[ $DEFAULTS == 1 ]]; then [[ $def == y ]] && printf -v "$var" 1 || printf -v "$var" 0; return; fi
  [[ $def == y ]] && hint="[Y/n]" || hint="[y/N]"
  while true; do
    read -r -p "$B$q$N $hint " ans || exit 1
    ans="${ans:-$def}"
    case "${ans,,}" in
      y|yes) printf -v "$var" 1; return ;;
      n|no) printf -v "$var" 0; return ;;
    esac
  done
}

# choose VAR "Question" DEFAULT_INDEX "label|description" ...: sets VAR to the index (1-based).
choose() {
  local var="$1" q="$2" def="$3"; shift 3
  if [[ $DEFAULTS == 1 ]]; then printf -v "$var" '%s' "$def"; return; fi
  say "$B$q$N"
  local i=1 opt
  for opt in "$@"; do
    printf '  %s%d)%s %s%s%s  %s%s%s\n' "$C" "$i" "$N" "$B" "${opt%%|*}" "$N" "$D" "${opt#*|}" "$N"
    i=$((i + 1))
  done
  local ans
  while true; do
    read -r -p "Choose [${def}]: " ans || exit 1
    ans="${ans:-$def}"
    if [[ "$ans" =~ ^[0-9]+$ ]] && ((ans >= 1 && ans < i)); then printf -v "$var" '%s' "$ans"; return; fi
  done
}

# ---------------------------------------------------------------- validators

is_email() { [[ "$1" =~ ^[^@[:space:]\<\>\",]+@[^@[:space:]\<\>\",]+$ ]] || { warn "that isn't an email address"; return 1; }; }
is_name() {
  [[ "$1" =~ ^[a-z][a-z0-9_]{0,62}$ && "$1" != goober_* ]] ||
    { warn "names are [a-z][a-z0-9_]*, at most 63, not starting with goober_"; return 1; }
}
is_domain() { [[ "$1" =~ ^[a-z0-9]([a-z0-9.-]*[a-z0-9])?$ ]] || { warn "use a lowercase domain, like example.com"; return 1; }; }
is_port() { [[ "$1" =~ ^[0-9]+$ ]] && (($1 >= 1 && $1 <= 65535)) || { warn "a port is 1 to 65535"; return 1; }; }
is_count() { [[ "$1" =~ ^[0-9]+$ ]] && (($1 >= 1)) || { warn "a whole number, at least 1"; return 1; }; }
is_url() { [[ "$1" =~ ^https?://[^/[:space:]]+$ ]] || { warn "like https://goober.example.com, with no path"; return 1; }; }
is_url_any() { [[ "$1" =~ ^smtps?://[A-Za-z0-9.:\[\]-]+$ ]] || { warn "like smtps://smtp.example.com:465"; return 1; }; }
port_busy() { (echo >"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
tenant_from_domain() {
  local t="${1%%.*}"; t="${t//-/_}"; t="${t,,}"
  [[ "$t" =~ ^[a-z] ]] || t="t_$t"
  printf '%s' "${t:0:63}"
}

# ---------------------------------------------------------------- start

[[ -x "$BIN" ]] || die "no goober-host at $BIN; build first (just build)"
command -v python3 >/dev/null || die "python3 is needed to write the config"

mkdir -p "$DIR"
DIR="$(cd "$DIR" && pwd)"
CONFIG="$DIR/host.json"

if [[ $DEFAULTS == 0 ]]; then
  cat <<EOF

${B}goober setup${N}
This writes a host config to ${B}$CONFIG${N} and makes its storage.
Every answer has a default; press Enter to take it, or edit it in place.
Nothing here is permanent: rerun ${B}just setup${N} any time.
EOF
fi

if [[ -f "$CONFIG" ]]; then
  if [[ $DEFAULTS == 1 ]]; then exit 0; fi
  step "An existing host"
  say "There is already a config at $CONFIG. Storage (content, boxes, identities) is kept either way."
  choose existing "What now?" 1 \
    "Reconfigure|answer the questions again; the old config is saved as host.json.bak-<time>" \
    "Keep it|leave everything as it is"
  if [[ $existing == 2 ]]; then good "kept $CONFIG"; exit 0; fi
  cp "$CONFIG" "$CONFIG.bak-$(date +%Y%m%d-%H%M%S)"
fi

# ---------------------------------------------------------------- 1. purpose

step "What is this host for?"
choose MODE "Pick the kind of host:" 1 \
  "A dev box|play on this machine: localhost, sign-in codes in the terminal, the examples deployed" \
  "Production|serve real domains from a server, behind a TLS proxy like Caddy"
PROD=$([[ $MODE == 2 ]] && echo 1 || echo 0)

# ---------------------------------------------------------------- 2. you

step "You"
note "Your email becomes a host operator with host:owner: the one role only this config can grant."
note "It is also the first owner of your first tenant. Signing in uses a code mailed to this address."
DEFAULT_EMAIL="$(git config user.email 2>/dev/null || true)"
DEFAULT_EMAIL="${DEFAULT_EMAIL:-me@example.com}"
ask EMAIL "Your email:" "$DEFAULT_EMAIL" is_email
EMAIL="${EMAIL,,}"

# ---------------------------------------------------------------- 3. network

step "Network"
if [[ $PROD == 1 ]]; then
  note "The host speaks plain HTTP. TLS, HTTP/2, and certificates belong to a proxy in front"
  note "(099.TheSystem.md: rent the edge). Keep the host on 127.0.0.1 and let the proxy reach it."
  ask ADDRESS "Listen address:" "127.0.0.1"
else
  note "A dev box listens on 127.0.0.1 only; nothing outside this machine can reach it."
  ADDRESS="127.0.0.1"
fi
PORT_DEFAULT=8080
while port_busy "$PORT_DEFAULT" && ((PORT_DEFAULT < 8100)); do PORT_DEFAULT=$((PORT_DEFAULT + 1)); done
[[ $PORT_DEFAULT != 8080 ]] && note "Port 8080 is busy right now, so the default is $PORT_DEFAULT."
ask PORT "Port:" "$PORT_DEFAULT" is_port

# ---------------------------------------------------------------- 4. domains

step "Domains and tenants"
note "A domain picks a tenant. A tenant owns its static content, its identity realm (users,"
note "sessions, roles), and later its databases. Two domains may share one tenant."
DOMAINS=()   # entries: domain|tenant|owner
if [[ $PROD == 0 ]]; then
  ask TENANT "Tenant for localhost and 127.0.0.1:" "playground" is_name
  DOMAINS+=("localhost|$TENANT|$EMAIL" "127.0.0.1|$TENANT|")
  note "Browsers send any *.localhost name to this machine, so a second tenant is one name away."
  confirm SECOND "Add a second tenant at second.localhost, to see realms kept apart?" y
  if [[ $SECOND == 1 ]]; then
    ask TENANT2 "Tenant for second.localhost:" "second" is_name
    DOMAINS+=("second.localhost|$TENANT2|$EMAIL")
  fi
  PLATFORM="localhost"
  PUBLIC_URL=""
else
  while true; do
    ask DOMAIN "A domain this host serves (like bobsplumbing.com):" "" is_domain
    ask TENANT "  Its tenant name:" "$(tenant_from_domain "$DOMAIN")" is_name
    ask OWNER "  Its first owner's email (granted \"owner\" at first sign-in):" "$EMAIL" is_email
    DOMAINS+=("$DOMAIN|$TENANT|${OWNER,,}")
    confirm MORE "Add another domain?" n
    [[ $MORE == 1 ]] || break
  done
  note "The platform domain is the host's own: operators sign in there, and mail comes from it."
  ask PLATFORM "Platform domain:" "${DOMAINS[0]%%|*}" is_domain
  ask PUBLIC_URL "Public URL (what the proxy serves):" "https://$PLATFORM" is_url
fi

# ---------------------------------------------------------------- 5. identity

step "Identity"
note "The built-in identity service signs people in with a six-digit code sent by email,"
note "and keeps one SQLite database per tenant under storage/identity/. It is careful, but"
note "young: fine for play and small stakes (013.Identity.md § A warning)."
OPERATORS=("$EMAIL")
ask EXTRA_OPS "Other host operators (comma-separated emails, or empty):" ""
IFS=',' read -r -a extra <<<"$EXTRA_OPS"
for e in "${extra[@]}"; do
  e="$(echo "$e" | tr -d '[:space:]')"; e="${e,,}"
  [[ -n "$e" ]] && { is_email "$e" && OPERATORS+=("$e") || die "not an email: $e"; }
done
ask SESSION_DAYS "Days a sign-in lasts:" "30" is_count
if [[ $PROD == 1 ]]; then
  COOKIE_SECURE=true
  note "Session cookies will be __Host- and Secure, so they only travel over HTTPS."
  note "Sign-in codes and boxes' mail go by email. Without a mailer, the dev mailer writes them to"
  note "storage/mail/outbox/ on the server: fine for you alone, useless for anyone else."
  note "Amazon SES sends through its API with AWS keys from the key store (set below)."
  confirm USE_SES "Send mail through Amazon SES?" y
  USE_SMTP=0
  if [[ $USE_SES == 1 ]]; then
    ask SES_FROM "Send from (an address or domain verified in SES):" "no-reply@$PLATFORM" is_email
    ask AWS_REGION "The SES region:" "us-east-1"
  else
    confirm USE_SMTP "Send mail through an SMTP server (TLS required)?" y
  fi
  if [[ $USE_SMTP == 1 ]]; then
    ask SMTP_URL "SMTP URL (smtps://host:465, or smtp://host:587 with STARTTLS):" "smtps://smtp.example.com:465" is_url_any
    ask SMTP_FROM "Send from:" "no-reply@$PLATFORM" is_email
    ask SMTP_USER "SMTP username (empty for none):" ""
    ask SMTP_PASS_FILE "A file holding only the SMTP password (kept out of the config):" "/etc/goober/smtp-password"
    note "The host's user must be able to read it: chown root:${GOOBER_USER:-$(id -un)} and chmod 0640."
  fi
else
  COOKIE_SECURE=false
  note "Local mode prints each sign-in code in the terminal running the host."
fi

note "The dumb mobile terminal (999.Mobile.md) signs in as your own app, acting as you, to"
note "show alarms, dashboards, and approvals on your phone and desktop."
confirm TERMINAL "Register the dumb terminal as an app in every realm?" y

# ---------------------------------------------------------------- 6. threads

step "Threads"
CORES="$(nproc 2>/dev/null || echo 4)"
note "Network threads only route; box threads run WASM. They never steal from each other."
note "This machine has $CORES cores."
if [[ $PROD == 1 ]]; then
  NET_DEFAULT=$(( CORES / 4 )); ((NET_DEFAULT < 1)) && NET_DEFAULT=1; ((NET_DEFAULT > 4)) && NET_DEFAULT=4
  BOX_DEFAULT=$(( CORES - NET_DEFAULT )); ((BOX_DEFAULT < 1)) && BOX_DEFAULT=1
  PIN_DEFAULT=$([[ $CORES -ge 4 ]] && echo y || echo n)
else
  NET_DEFAULT=2; BOX_DEFAULT=4; PIN_DEFAULT=n
fi
ask NET_THREADS "Network threads:" "$NET_DEFAULT" is_count
ask BOX_THREADS "Box threads:" "$BOX_DEFAULT" is_count
confirm PIN "Pin box threads to cores (keeps boxes hot; best on a dedicated server)?" "$PIN_DEFAULT"

# ---------------------------------------------------------------- 7. limits

step "Box limits"
note "Each box gets a host default, and a manifest may ask for more up to the host maximum:"
note "  timeout 1 s (max 10 s), fuel 100M (max 2B), memory 64 MiB (max 512 MiB), bodies 8 MiB."
confirm LIMIT_DEFAULTS "Use these limits?" y
T_DEF=1000; T_MAX=10000; F_DEF=100000000; F_MAX=2000000000
M_DEF=67108864; M_MAX=536870912; BODY_MAX=8388608
if [[ $LIMIT_DEFAULTS == 0 ]]; then
  ask T_DEF "timeout-ms default:" "$T_DEF" is_count;  ask T_MAX "timeout-ms max:" "$T_MAX" is_count
  ask F_DEF "fuel default:" "$F_DEF" is_count;       ask F_MAX "fuel max:" "$F_MAX" is_count
  ask M_DEF "memory-max-bytes default:" "$M_DEF" is_count; ask M_MAX "memory-max-bytes max:" "$M_MAX" is_count
  ask BODY_MAX "body-max-bytes:" "$BODY_MAX" is_count
fi

# ---------------------------------------------------------------- 8. operator access

step "Operator access"
note "Agents and tools operate the host over MCP at /goober-operations-mcp with a bearer token."
if [[ $PROD == 1 ]]; then
  note "Production keeps no token in the config. Mint one per agent with ${B}just mcp${N}${D};"
  note "only its hash is stored, under storage/operations/tokens/."
  CONFIG_TOKEN=""
else
  note "A dev box keeps one token in host.json, so tools/goober.py and just examples work"
  note "with nothing to paste. Claude Code gets its own token from ${B}just mcp${N}${D}."
  CONFIG_TOKEN="dev-$(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n')"
fi

# ---------------------------------------------------------------- 9. backups

S3_BUCKET=""
if [[ $PROD == 1 ]]; then
  step "Backups"
  note "Off the machine (663.Backups.md): a sealed archive every night (fourteen kept), the"
  note "databases' changed pages every fifteen minutes, and each attachment once, to S3."
  ask S3_BUCKET "S3 bucket for backups (empty for none):" ""
  if [[ -n "$S3_BUCKET" ]]; then
    ask S3_REGION "Its region:" "${AWS_REGION:-us-east-1}"
    ask S3_PREFIX "A prefix in the bucket, for this host:" "${PLATFORM}/"
  fi
  if [[ ${USE_SES:-0} == 1 || -n "$S3_BUCKET" ]]; then
    note "SES and S3 use AWS keys, kept in the host's key store (never in the config)."
    note "The IAM user needs ses:SendRawEmail, and s3 Get/Put/Delete/ListBucket on the bucket."
    confirm SET_AWS "Enter the AWS access keys now?" y
    if [[ $SET_AWS == 1 ]]; then
      read -r -p "AWS access key id: " AWS_KEY_ID
      read -r -s -p "AWS secret access key (hidden): " AWS_SECRET; echo
    fi
  fi
fi

# ---------------------------------------------------------------- review and write

export G_ADDRESS="$ADDRESS" G_PORT="$PORT" G_PLATFORM="$PLATFORM" G_PUBLIC_URL="$PUBLIC_URL"
export G_DOMAINS="$(printf '%s\n' "${DOMAINS[@]}")" G_OPERATORS="$(printf '%s\n' "${OPERATORS[@]}")"
export G_SMTP="${USE_SMTP:-0}" G_SMTP_URL="${SMTP_URL:-}" G_SMTP_FROM="${SMTP_FROM:-}" G_SMTP_USER="${SMTP_USER:-}"
export G_SMTP_PASS_FILE="${SMTP_PASS_FILE:-}"
export G_SES="${USE_SES:-0}" G_SES_FROM="${SES_FROM:-}" G_AWS_REGION="${AWS_REGION:-us-east-1}"
export G_S3_BUCKET="${S3_BUCKET:-}" G_S3_REGION="${S3_REGION:-}" G_S3_PREFIX="${S3_PREFIX:-}"
export G_PROD="$PROD" G_TOKEN="$CONFIG_TOKEN" G_COOKIE_SECURE="$COOKIE_SECURE" G_SESSION_DAYS="$SESSION_DAYS" G_TERMINAL="$TERMINAL"
export G_NET="$NET_THREADS" G_BOX="$BOX_THREADS" G_PIN="$PIN" G_BODY="$BODY_MAX"
export G_T_DEF="$T_DEF" G_T_MAX="$T_MAX" G_F_DEF="$F_DEF" G_F_MAX="$F_MAX" G_M_DEF="$M_DEF" G_M_MAX="$M_MAX"
JSON="$(python3 - <<'PY'
import json, os
e = os.environ
domains = {}
for line in e["G_DOMAINS"].splitlines():
    d, t, o = line.split("|")
    domains[d] = {"tenant": t, **({"owner": o} if o else {})}
cfg = {
    "listen": {"address": e["G_ADDRESS"], "port": int(e["G_PORT"]),
               **({"behind-proxy": True} if e["G_PROD"] == "1" else {})},
    "storage": "storage",
    "platform-domain": e["G_PLATFORM"],
    **({"public-url": e["G_PUBLIC_URL"]} if e["G_PUBLIC_URL"] else {}),
    "domains": domains,
    **({"operations": {"token": e["G_TOKEN"]}} if e["G_TOKEN"] else {}),
    "identity": {
        "cookie-secure": e["G_COOKIE_SECURE"] == "true",
        "session-days": int(e["G_SESSION_DAYS"]),
        "mailer": "ses" if e["G_SES"] == "1" else "smtp" if e["G_SMTP"] == "1" else "dev",
        **({"ses": {"from": e["G_SES_FROM"]}} if e["G_SES"] == "1" else {}),
        **({"smtp": {"url": e["G_SMTP_URL"], "from": e["G_SMTP_FROM"],
                     **({"username": e["G_SMTP_USER"], "password-file": e["G_SMTP_PASS_FILE"]} if e["G_SMTP_USER"] else {})}}
           if e["G_SMTP"] == "1" else {}),
        "operators": {o: ["host:owner"] for o in e["G_OPERATORS"].splitlines()},
        **({"clients": {"dumb-terminal": {
            "name": "dumb terminal",
            "redirect-uris": ["com.mathgladiator.dumb:/oauth/callback", "http://127.0.0.1/callback"],
            "acts-as": "person"}}} if e["G_TERMINAL"] == "1" else {}),
    },
    **({"aws": {"region": e["G_AWS_REGION"]}} if e["G_SES"] == "1" else {}),
    **({"backup": {"s3": {"bucket": e["G_S3_BUCKET"], "region": e["G_S3_REGION"], "prefix": e["G_S3_PREFIX"]}}}
       if e["G_S3_BUCKET"] else {}),
    "threads": {"network": int(e["G_NET"]), "boxes": int(e["G_BOX"]), "pin-boxes": e["G_PIN"] == "1"},
    "limits": {
        "body-max-bytes": int(e["G_BODY"]),
        "timeout-ms": {"default": int(e["G_T_DEF"]), "max": int(e["G_T_MAX"])},
        "fuel": {"default": int(e["G_F_DEF"]), "max": int(e["G_F_MAX"])},
        "memory-max-bytes": {"default": int(e["G_M_DEF"]), "max": int(e["G_M_MAX"])},
    },
}
print(json.dumps(cfg, indent=2))
PY
)"

step "Review"
say "$JSON" | sed "s/\"dev-[0-9a-f]*\"/\"dev-…(hidden)\"/"
confirm WRITE "Write this to $CONFIG?" y
[[ $WRITE == 1 ]] || die "nothing written"

umask 077
printf '%s\n' "$JSON" >"$CONFIG"
mkdir -p "$DIR/storage"
# A dev box with cargo builds Rust boxes from source projects: vendor the crates they may use.
if [[ $PROD != 1 ]] && command -v cargo >/dev/null && "$HERE/vendor.sh" "$DIR/vendor" >/dev/null 2>&1; then
  python3 -c '
import json, sys
c = json.load(open(sys.argv[1]))
c["source"] = {"vendor": "vendor"}
open(sys.argv[1], "w").write(json.dumps(c, indent=2) + "\n")
' "$CONFIG"
  good "vendored the Rust crates boxes may use into $DIR/vendor"
fi
# The AWS keys go into the key store through stdin, never a command line.
if [[ -n "${AWS_KEY_ID:-}" && -n "${AWS_SECRET:-}" ]]; then
  printf '%s' "$AWS_KEY_ID" | "$BIN" key set aws_access_key_id --config "$CONFIG" >/dev/null
  printf '%s' "$AWS_SECRET" | "$BIN" key set aws_secret_access_key --config "$CONFIG" >/dev/null
  unset AWS_SECRET
  good "stored the AWS keys in the key store"
fi
if [[ -n "${S3_BUCKET:-}" ]]; then
  BACKUP_KEY="$("$BIN" backup key --config "$CONFIG" 2>/dev/null)"
  warn "The backup key (keep a copy OFF this machine, in a password manager; no backup can be read without it):"
  say "  $BACKUP_KEY"
  note "Once it's stored safely: $BIN backup key --confirm --config $CONFIG"
fi
"$BIN" check --config "$CONFIG" >/dev/null || die "the host refused the config it was just given; see above"
good "wrote $CONFIG"
good "storage at $DIR/storage"

# ---------------------------------------------------------------- production files

if [[ $PROD == 1 ]]; then
  step "Production files"
  mkdir -p "$DIR/deploy"
  cat >"$DIR/deploy/goober-host.service" <<EOF
# systemd unit for this host. Install with:
#   sudo cp $DIR/deploy/goober-host.service /etc/systemd/system/
#   sudo systemctl daemon-reload && sudo systemctl enable --now goober-host
# Reload the config with: sudo systemctl reload goober-host
[Unit]
Description=goober host ($DIR)
After=network-online.target
Wants=network-online.target

[Service]
User=${GOOBER_USER:-$(id -un)}
WorkingDirectory=$DIR
ExecStart=$BIN --config $CONFIG
ExecReload=/bin/kill -HUP \$MAINPID
Restart=on-failure
RestartSec=2
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ReadWritePaths=$DIR
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
EOF
  {
    echo "# Caddy terminates TLS (with automatic certificates) and hands every request"
    echo "# to the host. Install Caddy, then: sudo cp $DIR/deploy/Caddyfile /etc/caddy/Caddyfile"
    sites="$PLATFORM"
    for entry in "${DOMAINS[@]}"; do
      d="${entry%%|*}"; [[ "$d" != "$PLATFORM" ]] && sites="$sites, $d"
    done
    echo "$sites {"
    echo "  encode zstd gzip"
    echo "  reverse_proxy $ADDRESS:$PORT {"
    echo "    health_uri /goober/health"
    echo "    # A restart takes a moment: hold requests for it rather than answer 503 at once."
    echo "    lb_try_duration 15s"
    echo "    lb_try_interval 250ms"
    echo "  }"
    echo "  # The host itself down: a retryable answer, so clients try again rather than cache a failure."
    echo "  handle_errors {"
    echo "    @down expression \`{err.status_code} in [502, 503, 504]\`"
    echo "    handle @down {"
    echo "      header Retry-After 15"
    echo "      header Cache-Control no-store"
    echo "      header Content-Type application/json"
    echo "      respond \`{\"error\":\"temporarily_unavailable\",\"error_description\":\"the host is restarting; retry shortly\"}\` 503"
    echo "    }"
    echo "  }"
    echo "}"
  } >"$DIR/deploy/Caddyfile"
  # A nightly backup, kept for two weeks: SQLite's online backup, safe while the host runs.
  cat >"$DIR/deploy/goober-backup.service" <<UNIT
[Unit]
Description=Back up the goober host ($DIR)

[Service]
Type=oneshot
User=${GOOBER_USER:-$(id -un)}
ExecStart=/bin/sh -c '$BIN backup "$DIR/backups/\$\$(date +%%F)" --config "$CONFIG" && find "$DIR/backups" -mindepth 1 -maxdepth 1 -type d -mtime +14 -exec rm -rf {} +'
UNIT
  cat >"$DIR/deploy/goober-backup.timer" <<UNIT
# Install next to goober-host.service, then: sudo systemctl enable --now goober-backup.timer
[Unit]
Description=Nightly goober backup

[Timer]
OnCalendar=*-*-* 03:15:00
Persistent=true

[Install]
WantedBy=timers.target
UNIT
  good "wrote $DIR/deploy/goober-host.service"
  good "wrote $DIR/deploy/goober-backup.service and .timer (nightly, two weeks kept)"
  good "wrote $DIR/deploy/Caddyfile"
  if [[ "$BIN" == */build/dev/* ]]; then
    note "The host binary is the dev build at $BIN. For a server, build the server preset"
    note "(cmake --preset server && cmake --build --preset server), or use tools/deploy.sh."
  fi
fi

# ---------------------------------------------------------------- examples

step "Something to look at"
confirm EXAMPLES "Deploy the example site and boxes (dice, echo, identity demos)?" "$([[ $PROD == 1 ]] && echo n || echo y)"
if [[ $EXAMPLES == 1 ]]; then
  if port_busy "$PORT"; then
    warn "port $PORT is in use, so the examples can't be seeded now."
    warn "Once your host is running, run: just examples $DIR"
  else
    LOG="$DIR/storage/setup-seed.log"
    "$BIN" --config "$CONFIG" --local >"$LOG" 2>&1 &
    SEED_PID=$!
    trap 'kill $SEED_PID 2>/dev/null || true' EXIT
    for _ in $(seq 1 50); do port_busy "$PORT" && break; sleep 0.1; done
    FIRST_TENANT="$(cut -d'|' -f2 <<<"${DOMAINS[0]}")"
    if "$HERE/examples.sh" "$CONFIG" "$FIRST_TENANT" >>"$LOG" 2>&1; then
      good "examples deployed"
    else
      warn "deploying the examples failed; see $LOG"
    fi
    kill "$SEED_PID" 2>/dev/null || true
    wait "$SEED_PID" 2>/dev/null || true
    trap - EXIT
  fi
fi

# ---------------------------------------------------------------- done

step "Done"
if [[ $PROD == 1 ]]; then
  cat <<EOF
Next:
  ${B}just mcp $DIR${N}             mint an operator token and print the claude command
  ${B}just serve $DIR${N}           run it in the foreground to try it (no --local)
  install ${B}$DIR/deploy/${N} for systemd and Caddy, and point DNS at this server
  sign in as an operator at ${B}$PUBLIC_URL/identity/sign-in/as-host${N}; $(
  [[ ${USE_SMTP:-0} == 1 || ${USE_SES:-0} == 1 ]] && echo "the code comes by email" || echo "the code is in $DIR/storage/mail/outbox/")
EOF
else
  cat <<EOF
Next:
  ${B}just run${N}                  start the host (local mode)
  open ${B}http://localhost:$PORT/${N}
  sign in at ${B}http://localhost:$PORT/identity/sign-in${N} as $EMAIL;
      the code prints in the terminal running the host
  ${B}just mcp${N}                  mint a token for Claude Code and print the command
EOF
fi

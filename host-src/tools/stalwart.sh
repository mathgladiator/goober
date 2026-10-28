#!/usr/bin/env bash
# Installs and provisions Stalwart (https://github.com/stalwartlabs/stalwart) as goober's
# inbound-only SMTP front end: it listens for SMTP and hands every recipient and every message
# to goober's MTA hook, http://127.0.0.1:<port>/internal/routes/mta/stalwart. It never
# delivers mail anywhere: the hook answers RCPT, then DATA with discard or reject.
#
#   tools/stalwart.sh install DIR
#   tools/stalwart.sh configure DIR --smtp ADDR:PORT --admin ADDR:PORT --hook-url URL
#                     --hook-token-file PATH [--max-bytes N] [--hostname NAME]
#                     [--relay-domains a.example,b.example] [--cert FILE --key FILE]
#   tools/stalwart.sh run DIR                (foreground; tests, or by hand)
#   tools/stalwart.sh systemd DIR USER       (prints a unit file)
#   tools/stalwart.sh reload-tls DIR         (re-reads --cert/--key in a running Stalwart)
#
# DIR holds everything: bin/stalwart, etc/config.json (where the data store is),
# etc/stalwart.env (the admin credential, 0600), and data/ (RocksDB). Run configure, run and
# reload-tls as the user Stalwart runs as, never as root: Stalwart writes its data as that user.
#
# configure is safe to rerun, and must run while Stalwart is stopped. It starts a temporary
# Stalwart in recovery mode, writes every setting through Stalwart's JMAP API, and stops it.
# Recovery mode listens on [::]:PORT (all interfaces, Stalwart's choice), so configure runs it
# inside a private network namespace (unshare -rn) when the kernel allows that; otherwise it
# warns, and the admin port is open on every interface for the seconds configure takes.
#
# How mail is kept inbound only (each layer holds even if the one before it fails):
#   1. The hook runs at RCPT and refuses every address goober doesn't serve. If goober is
#      down or answers badly, Stalwart answers 451 (tempFailOnError).
#   2. Stalwart's relay rule allows only --relay-domains, or every domain when none are
#      given (the hook alone decides then).
#   3. The hook runs at DATA and answers discard (250, nothing kept) or reject.
#   4. A queue quota of one byte: anything Stalwart would queue (a hook that answered
#      accept, a missing hook, a report, a bounce) is refused with 452 4.3.1 instead.
#   5. Every outbound route is local, every report is off, and SMTP AUTH is off.
set -euo pipefail

main() {

VERSION="1.0.1"
ASSET="stalwart-x86_64-unknown-linux-gnu.tar.gz"
SHA256="aabae69bcdeff31e305d60f3ecb059fdb203116c0077d81560bf6d6f0f757d75"
URL="https://github.com/stalwartlabs/stalwart/releases/download/v$VERSION/$ASSET"

die() { echo "✗ $*" >&2; exit 1; }
usage() { sed -n '7,13p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

CMD="${1:-}"
[[ -n "$CMD" ]] || usage
shift
case "$CMD" in
  install) install_stalwart "$@" ;;
  configure) configure "$@" ;;
  run) run_stalwart "$@" ;;
  systemd) print_unit "$@" ;;
  reload-tls) reload_tls "$@" ;;
  __provision) provision_inside "$@" ;;
  -h|--help|help) usage ;;
  *) echo "✗ unknown command $CMD" >&2; usage ;;
esac
}

abs_dir() {
  local dir="${1:?DIR is required}"
  [[ "$dir" != *$'\n'* ]] || die "DIR has a newline in it"
  mkdir -p -- "$dir"
  (cd -- "$dir" && pwd -P)
}

not_root() {
  [[ $EUID -ne 0 ]] || die "run this as the user Stalwart runs as, not root (runuser -u stalwart -- $0 ...)"
}

# ── install DIR
install_stalwart() {
  [[ $# -eq 1 ]] || usage
  [[ "$(uname -s)/$(uname -m)" == Linux/x86_64 ]] || die "this installs the x86_64 Linux build"
  local dir; dir="$(abs_dir "$1")"
  local bin="$dir/bin/stalwart"
  if [[ -x "$bin" && "$("$bin" --version 2>/dev/null)" == "$VERSION" ]]; then
    echo "── Stalwart $VERSION is already in $dir/bin"
    return
  fi
  echo "── downloading Stalwart $VERSION"
  mkdir -p -- "$dir/bin"
  TMP_DOWNLOAD="$(mktemp -d "$dir/.download.XXXXXXXX")"
  trap 'rm -rf -- "$TMP_DOWNLOAD"' EXIT
  local tmp="$TMP_DOWNLOAD"
  curl -fsSL --proto '=https' --tlsv1.2 -o "$tmp/$ASSET" "$URL"
  echo "$SHA256  $tmp/$ASSET" | sha256sum --check --quiet - ||
    die "the download's sha256 isn't $SHA256; not installing it"
  # The archive is untrusted until checked; even then only the one expected file is taken,
  # into its own empty directory.
  mkdir "$tmp/x"
  tar -xzf "$tmp/$ASSET" -C "$tmp/x" --no-same-owner --no-same-permissions stalwart
  [[ -f "$tmp/x/stalwart" && ! -L "$tmp/x/stalwart" ]] || die "the archive has no stalwart binary"
  install -m 0755 "$tmp/x/stalwart" "$bin.new"
  mv -f -- "$bin.new" "$bin"
  [[ "$("$bin" --version)" == "$VERSION" ]] || die "the installed binary doesn't say it is $VERSION"
  echo "── installed $bin ($VERSION, sha256 $SHA256)"
}

# ── configure DIR ...
configure() {
  not_root
  [[ $# -ge 1 ]] || usage
  local dir; dir="$(abs_dir "$1")"; shift
  local smtp="" admin="" hook_url="" token_file="" max_bytes=26214400 hostname="" relay="" cert="" key=""
  while (($#)); do
    case "$1" in
      --smtp) smtp="${2:?--smtp ADDR:PORT}"; shift ;;
      --admin) admin="${2:?--admin ADDR:PORT}"; shift ;;
      --hook-url) hook_url="${2:?--hook-url URL}"; shift ;;
      --hook-token-file) token_file="${2:?--hook-token-file PATH}"; shift ;;
      --max-bytes) max_bytes="${2:?--max-bytes N}"; shift ;;
      --hostname) hostname="${2:?--hostname NAME}"; shift ;;
      --relay-domains) relay="${2:?--relay-domains a.example,b.example}"; shift ;;
      --cert) cert="${2:?--cert FILE}"; shift ;;
      --key) key="${2:?--key FILE}"; shift ;;
      *) die "unknown option $1" ;;
    esac
    shift
  done
  local addr_re='^([0-9]{1,3}(\.[0-9]{1,3}){3}|\[[0-9A-Fa-f:.]+\]):[0-9]{1,5}$'
  [[ "$smtp" =~ $addr_re ]] || die "--smtp must be ADDR:PORT, like 0.0.0.0:25 or [::]:25"
  [[ "$admin" =~ $addr_re ]] || die "--admin must be ADDR:PORT, like 127.0.0.1:8025"
  [[ "$admin" == 127.* || "$admin" == "[::1]":* ]] || die "--admin must be a loopback address: it is Stalwart's admin API"
  [[ "$hook_url" =~ ^https?://[^[:space:]]+$ ]] || die "--hook-url must be an http(s) URL"
  [[ "$token_file" == /* && -f "$token_file" && -r "$token_file" && -s "$token_file" ]] ||
    die "--hook-token-file must be an absolute path to a readable, non-empty file"
  [[ "$max_bytes" =~ ^[1-9][0-9]{0,11}$ ]] || die "--max-bytes must be a positive number of bytes"
  [[ -z "$relay" || "$relay" =~ ^[a-z0-9.-]+(,[a-z0-9.-]+)*$ ]] ||
    die "--relay-domains must be lowercase domain names separated by commas"
  [[ -z "$cert" && -z "$key" ]] || [[ "$cert" == /* && -r "$cert" && "$key" == /* && -r "$key" ]] ||
    die "--cert and --key go together, as absolute paths to readable PEM files"
  hostname="${hostname:-$(hostname -f 2>/dev/null || hostname)}"
  [[ "$hostname" =~ ^[A-Za-z0-9.-]+$ ]] || die "--hostname must be a host name"
  local smtp_port="${smtp##*:}" admin_port="${admin##*:}"
  ((smtp_port >= 1 && smtp_port <= 65535 && admin_port >= 1 && admin_port <= 65535)) || die "ports run 1..65535"
  [[ "$smtp_port" != "$admin_port" ]] || die "--smtp and --admin need different ports"

  local bin="$dir/bin/stalwart"
  [[ -x "$bin" ]] || die "no Stalwart in $dir/bin; run: $0 install $dir"
  if pgrep -f -- "^$bin --config[= ]$dir/etc/config.json" >/dev/null 2>&1; then
    die "Stalwart from $dir is running; stop it first (systemctl stop stalwart), then rerun"
  fi

  echo "── files in $dir"
  umask 077
  mkdir -p -- "$dir/etc" "$dir/data"
  chmod 0700 "$dir/etc" "$dir/data"
  if [[ ! -f "$dir/etc/config.json" ]]; then
    DATA="$dir/data" python3 -I -c 'import json, os; print(json.dumps({"@type": "RocksDb", "path": os.environ["DATA"]}))' \
      > "$dir/etc/config.json"
  fi
  # The admin credential is made once, and kept across reruns. Stalwart accepts it as its
  # recovery administrator (STALWART_RECOVERY_ADMIN), in recovery mode and in normal mode.
  local password=""
  if [[ -f "$dir/etc/stalwart.env" ]]; then
    password="$(sed -n 's/^STALWART_RECOVERY_ADMIN=admin://p' "$dir/etc/stalwart.env" | tail -n 1)"
  fi
  if [[ -z "$password" ]]; then
    password="$(head -c 32 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' | head -c 32)"
  fi
  [[ ${#password} -ge 24 ]] || die "couldn't make an admin password"
  cat > "$dir/etc/stalwart.env.new" <<EOF
# Written by goober's tools/stalwart.sh configure; rerun it rather than editing this.
STALWART_RECOVERY_ADMIN=admin:$password
STALWART_HOSTNAME=$hostname
GOOBER_STALWART_ADMIN=$admin
EOF
  mv -f -- "$dir/etc/stalwart.env.new" "$dir/etc/stalwart.env"

  echo "── provisioning (a temporary Stalwart in recovery mode)"
  export GS_DIR="$dir" GS_SMTP="$smtp" GS_ADMIN="$admin" GS_HOOK_URL="$hook_url" GS_TOKEN_FILE="$token_file" \
    GS_MAX_BYTES="$max_bytes" GS_HOSTNAME="$hostname" GS_RELAY="$relay" GS_CERT="$cert" GS_KEY="$key"
  local self; self="$(realpath -- "$0")"
  if [[ "${STALWART_PRIVATE_NETNS:-}" == 1 ]]; then
    # The caller (server-bootstrap.sh, as root) already put this in a private network namespace.
    "$self" __provision shared
  elif command -v unshare >/dev/null && command -v ip >/dev/null &&
     unshare -rn -- ip link set lo up >/dev/null 2>&1; then
    unshare -rn -- "$self" __provision netns
  else
    echo "  ! no private network namespace here (unshare -rn failed); recovery mode will listen on" >&2
    echo "    [::]:$admin_port, every interface, while configure runs. Block that port in the firewall." >&2
    "$self" __provision shared
  fi
  echo "── done: SMTP on $smtp, hook $hook_url, admin API on $admin"
  echo "   start it: $0 run $dir   (or install the unit: $0 systemd $dir USER)"
}

# Runs the temporary recovery-mode Stalwart and the provisioning client side by side; inside
# a private network namespace when called with "netns".
provision_inside() {
  local where="${1:-}"
  local dir="${GS_DIR:?}"
  local port="${GS_ADMIN##*:}"
  if [[ "$where" == netns ]]; then
    ip link set lo up
  elif (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
    die "something already listens on port $port; --admin needs a free port"
  fi
  local log="$dir/etc/provision.log"
  (
    set -a
    # shellcheck disable=SC1091
    . "$dir/etc/stalwart.env"
    set +a
    export STALWART_RECOVERY_MODE=true STALWART_RECOVERY_MODE_PORT="$port"
    exec "$dir/bin/stalwart" --config "$dir/etc/config.json"
  ) > "$log" 2>&1 &
  STALWART_PID=$!
  trap 'kill "$STALWART_PID" 2>/dev/null; wait "$STALWART_PID" 2>/dev/null || true' EXIT
  local pid="$STALWART_PID"
  # Up to a minute: in the private namespace there is no DNS, and Stalwart's start-up probe of
  # the resolver takes about 15 seconds to give up.
  local ok=""
  for _ in $(seq 1 600); do
    if ! kill -0 "$pid" 2>/dev/null; then break; fi
    if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then ok=1; break; fi
    sleep 0.1
  done
  [[ -n "$ok" ]] || { cat "$log" >&2; die "the temporary Stalwart didn't start (log above)"; }
  GS_URL="http://127.0.0.1:$port" python3 -I - <<'PY'
import base64, json, os, sys, urllib.error, urllib.request

env = os.environ
password = None
with open(os.path.join(env["GS_DIR"], "etc", "stalwart.env")) as f:
    for line in f:
        if line.startswith("STALWART_RECOVERY_ADMIN=admin:"):
            password = line.split(":", 1)[1].strip()
AUTH = "Basic " + base64.b64encode(b"admin:" + password.encode()).decode()
USING = ["urn:ietf:params:jmap:core", "urn:stalwart:jmap"]


def call(*calls):
    body = json.dumps({"using": USING, "methodCalls": [[m, a, str(i)] for i, (m, a) in enumerate(calls)]}).encode()
    req = urllib.request.Request(env["GS_URL"] + "/jmap/", data=body, method="POST",
                                 headers={"content-type": "application/json", "authorization": AUTH})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            out = json.loads(r.read())["methodResponses"]
    except urllib.error.HTTPError as e:
        sys.exit(f"✗ Stalwart answered {e.code}: {e.read().decode(errors='replace')}")
    for name, args, _ in out:
        if name == "error":
            sys.exit(f"✗ Stalwart refused a call: {json.dumps(args)}")
        for bad in ("notCreated", "notUpdated", "notDestroyed"):
            if args.get(bad):
                sys.exit(f"✗ {name} {bad}: {json.dumps(args[bad])}")
    return [args for _, args, _ in out]


def expr(value, matches=()):
    return {"else": value, "match": {str(i): {"if": c, "then": t} for i, (c, t) in enumerate(matches)}}


def singleton(obj, patch):
    call((f"x:{obj}/set", {"update": {"singleton": patch}}))


def reconcile(obj, wanted, same):
    """Makes the objects of type obj exactly `wanted`: updates the ones `same` matches,
    creates the missing, destroys the rest. Returns the ids, in wanted's order."""
    have = call((f"x:{obj}/get", {}))[0]["list"]
    ids, update, create, used = [], {}, {}, set()
    for i, w in enumerate(wanted):
        match = next((h for h in have if h["id"] not in used and same(h, w)), None)
        if match:
            used.add(match["id"])
            patch = {k: v for k, v in w.items() if match.get(k) != v}
            if patch:
                update[match["id"]] = patch
            ids.append(match["id"])
        else:
            create[f"n{i}"] = w
            ids.append(f"n{i}")
    destroy = [h["id"] for h in have if h["id"] not in used]
    args = {}
    if update: args["update"] = update
    if create: args["create"] = create
    if destroy: args["destroy"] = destroy
    if args:
        res = call((f"x:{obj}/set", args))[0]
        created = res.get("created") or {}
        ids = [created[i]["id"] if i in created else i for i in ids]
    return ids


def bind(addr):
    return {addr: True}


# Listeners: SMTP where asked, the admin API on loopback, nothing else. Having any listener
# also keeps Stalwart from adding its defaults (25, 465, 993, 995, 4190, 443, 8080 on [::]).
reconcile("NetworkListener", [
    {"name": "smtp", "bind": bind(env["GS_SMTP"]), "protocol": "smtp", "useTls": True,
     "implicitTls": False, "socketReusePort": False},
    {"name": "admin", "bind": bind(env["GS_ADMIN"]), "protocol": "http", "useTls": False,
     "implicitTls": False, "socketReusePort": False},
], lambda h, w: h["name"] == w["name"])

# The one hook, at RCPT and DATA. The token stays in its file; Stalwart reads it at start.
reconcile("MtaHook", [{
    "url": env["GS_HOOK_URL"],
    "stages": {"rcpt": True, "data": True},
    "enable": expr("true"),
    "tempFailOnError": True,
    "timeout": 30000,
    "maxResponseSize": 1048576,
    "allowInvalidCerts": False,
    "httpAuth": {"@type": "Bearer", "bearerToken": {"@type": "File", "filePath": env["GS_TOKEN_FILE"]}},
    "httpHeaders": {},
}], lambda h, w: True)

# Relaying: the hook decides at RCPT, and every domain goober serves is foreign to Stalwart,
# so the relay rule has to let them through. With --relay-domains only those can be.
domains = [d for d in env["GS_RELAY"].split(",") if d]
relay = " || ".join(f"rcpt_domain == '{d}'" for d in domains) if domains else ""
singleton("MtaStageRcpt", {"allowRelaying": expr("false", [(relay, "true")]) if relay else expr("true")})

singleton("MtaStageData", {
    "maxMessageSize": expr(env["GS_MAX_BYTES"]),
    "enableSpamFilter": expr("false"),
})
# No SMTP AUTH on any port: nobody signs in to send.
singleton("MtaStageAuth", {"saslMechanisms": expr("false"), "require": expr("false")})
# The same checks on every port (Stalwart's defaults key them to port 25), so tests on a high
# port see what production sees. Their results only reach Stalwart's logs: see the notes.
singleton("SenderAuth", {
    "dkimVerify": expr("relaxed"), "spfEhloVerify": expr("relaxed"), "spfFromVerify": expr("relaxed"),
    "dmarcVerify": expr("relaxed"), "reverseIpVerify": expr("relaxed"), "arcVerify": expr("disable"),
})
# Nothing outbound: no reports of any kind, no report interception, every route local.
singleton("DmarcReportSettings", {"aggregateSendFrequency": expr("disable"), "failureSendFrequency": expr("false")})
singleton("SpfReportSettings", {"sendFrequency": expr("false")})
singleton("DkimReportSettings", {"sendFrequency": expr("false")})
singleton("TlsReportSettings", {"sendFrequency": expr("disable")})
singleton("ReportSettings", {"inboundReportAddresses": {}})
singleton("MtaOutboundStrategy", {"route": expr("'local'"), "schedule": expr("'local'")})
# The queue holds nothing: one byte is less than any message.
reconcile("MtaQueueQuota", [{"description": "goober: Stalwart never queues mail", "enable": True,
                             "messages": 1, "size": 1, "key": {}}], lambda h, w: True)
# No spam filter (its verdict never reaches the hook), no greylisting, no rule downloads.
singleton("SpamSettings", {"enable": False, "spamFilterRulesUrl": None, "greylistFor": None})
# No downloads at start: the ASN/geo database and the web admin. (Asn "Disabled" is its
# default, which Stalwart doesn't store, and then it adds the 100 MB download at the next
# start; a download with no URLs is stored and fetches nothing.)
singleton("Asn", {"@type": "Resource", "asnUrls": {}, "geoUrls": {}})
apps = call(("x:Application/get", {}))[0]["list"]
if apps:
    call(("x:Application/set", {"update": {a["id"]: {"enable": False} for a in apps}}))
# Logs to stdout (the journal under systemd), not /var/log/stalwart.
reconcile("Tracer", [{"@type": "Stdout", "enable": True, "level": "info", "ansi": False}],
          lambda h, w: h.get("@type") == "Stdout")

# TLS for STARTTLS from files (Stalwart re-reads them at start and on reload-tls). It is the
# only certificate, so Stalwart serves it for every name, and to clients that send no SNI.
# Without one, Stalwart offers STARTTLS with a self-signed certificate. The host name in the
# greeting comes from STALWART_HOSTNAME in etc/stalwart.env: SystemSettings would need a
# Domain, and a Domain would make Stalwart answer 550 for its addresses itself.
wanted = []
if env["GS_CERT"]:
    wanted = [{"certificate": {"@type": "File", "filePath": env["GS_CERT"]},
               "privateKey": {"@type": "File", "filePath": env["GS_KEY"]}}]
reconcile("Certificate", wanted, lambda h, w: True)
print("  ✓ listeners, hook, relay rule, limits, reports, queue quota, spam, TLS")
PY
}

# ── run DIR
run_stalwart() {
  not_root
  [[ $# -eq 1 ]] || usage
  local dir; dir="$(abs_dir "$1")"
  [[ -f "$dir/etc/stalwart.env" && -f "$dir/etc/config.json" ]] || die "configure $dir first"
  set -a
  # shellcheck disable=SC1091
  . "$dir/etc/stalwart.env"
  set +a
  unset STALWART_RECOVERY_MODE
  exec "$dir/bin/stalwart" --config "$dir/etc/config.json"
}

# ── systemd DIR USER
print_unit() {
  [[ $# -eq 2 ]] || usage
  local dir="$1" user="$2"
  [[ "$dir" == /* && "$dir" =~ ^[A-Za-z0-9/._-]+$ ]] || die "DIR must be an absolute path"
  [[ "$user" =~ ^[a-z_][a-z0-9_-]{0,31}$ && "$user" != root ]] || die "USER must be a system user other than root"
  cat <<EOF
# /etc/systemd/system/stalwart.service: goober's inbound SMTP front end.
# Written by tools/stalwart.sh systemd $dir $user
[Unit]
Description=Stalwart (inbound SMTP for goober)
Conflicts=postfix.service sendmail.service exim4.service
After=network-online.target
Wants=network-online.target

[Service]
Type=notify
User=$user
Group=$user
EnvironmentFile=$dir/etc/stalwart.env
ExecStart=$dir/bin/stalwart --config=$dir/etc/config.json
KillMode=process
KillSignal=SIGINT
Restart=on-failure
RestartSec=5
LimitNOFILE=65536
SyslogIdentifier=stalwart
# Port 25 without root.
AmbientCapabilities=CAP_NET_BIND_SERVICE
CapabilityBoundingSet=CAP_NET_BIND_SERVICE
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectControlGroups=yes
RestrictSUIDSGID=yes
ReadWritePaths=$dir/data

[Install]
WantedBy=multi-user.target
EOF
}

# ── reload-tls DIR
reload_tls() {
  not_root
  [[ $# -eq 1 ]] || usage
  local dir; dir="$(abs_dir "$1")"
  GS_DIR="$dir" python3 -I - <<'PY'
import base64, json, os, sys, urllib.request
values = {}
with open(os.path.join(os.environ["GS_DIR"], "etc", "stalwart.env")) as f:
    for line in f:
        if "=" in line and not line.startswith("#"):
            k, v = line.rstrip("\n").split("=", 1)
            values[k] = v
auth = "Basic " + base64.b64encode(values["STALWART_RECOVERY_ADMIN"].encode()).decode()
body = json.dumps({"using": ["urn:ietf:params:jmap:core", "urn:stalwart:jmap"], "methodCalls": [
    ["x:Action/set", {"create": {"r": {"@type": "ReloadTlsCertificates"}}}, "0"]]}).encode()
req = urllib.request.Request(f"http://{values['GOOBER_STALWART_ADMIN']}/jmap/", data=body, method="POST",
                             headers={"content-type": "application/json", "authorization": auth})
with urllib.request.urlopen(req, timeout=30) as r:
    out = json.loads(r.read())["methodResponses"][0]
if out[0] == "error" or out[1].get("notCreated"):
    sys.exit("✗ reload failed: " + json.dumps(out[1]))
print("── certificates reloaded")
PY
}

main "$@"

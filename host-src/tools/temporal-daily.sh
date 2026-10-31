#!/usr/bin/env bash
# The daily check that timers keep real time (design/machine/014.Time.md § 8).
#
#   temporal-daily.sh            builds goober-temporal, runs the real timer driver for
#                                $GOOBER_TEMPORAL_MINUTES (default 5) minutes, and keeps its log
#   temporal-daily.sh install    installs a systemd user timer that runs it once a day
#   temporal-daily.sh uninstall  removes that timer
#
# Logs go to $GOOBER_TEMPORAL_LOGS (default <repo>/temporal-logs, ignored by git): one
# temporal-<start>.jsonl per run, a line per fire (expected, actual, drift_ms, verdict), and
# summary.jsonl with every run's summary line. Runs older than 90 days are deleted.
# Exits 0 when the run passed, 1 when it didn't.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST_SRC="$(dirname "$HERE")"
REPO="$(dirname "$HOST_SRC")"
LOGS="${GOOBER_TEMPORAL_LOGS:-$REPO/temporal-logs}"
MINUTES="${GOOBER_TEMPORAL_MINUTES:-5}"
UNITS="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"

case "${1:-run}" in
  install)
    mkdir -p "$UNITS"
    cat >"$UNITS/goober-temporal.service" <<EOF
[Unit]
Description=goober: check that timers keep real time

[Service]
Type=oneshot
ExecStart=$HERE/temporal-daily.sh
EOF
    cat >"$UNITS/goober-temporal.timer" <<EOF
[Unit]
Description=goober: the daily timer check

[Timer]
OnCalendar=daily
RandomizedDelaySec=1h
Persistent=true

[Install]
WantedBy=timers.target
EOF
    systemctl --user daemon-reload
    systemctl --user enable --now goober-temporal.timer
    echo "installed: runs daily; logs in $LOGS"
    echo "see it with: systemctl --user list-timers goober-temporal.timer"
    exit 0
    ;;
  uninstall)
    systemctl --user disable --now goober-temporal.timer 2>/dev/null || true
    rm -f "$UNITS/goober-temporal.service" "$UNITS/goober-temporal.timer"
    systemctl --user daemon-reload
    echo "removed; the logs in $LOGS are kept"
    exit 0
    ;;
  run) ;;
  *)
    echo "usage: temporal-daily.sh [install|uninstall]" >&2
    exit 2
    ;;
esac

export PATH="$HOME/.local/bin:$PATH"
cd "$HOST_SRC"
cmake --preset dev >/dev/null
cmake --build --preset dev --target goober-temporal >/dev/null
mkdir -p "$LOGS"
status=0
summary="$("$HOST_SRC/build/dev/packages/host/goober-temporal" --minutes "$MINUTES" --out "$LOGS")" || status=$?
echo "$summary"
[ -n "$summary" ] && echo "$summary" >>"$LOGS/summary.jsonl"
find "$LOGS" -maxdepth 1 -name 'temporal-*.jsonl' -mtime +90 -delete
exit "$status"

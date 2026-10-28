#!/usr/bin/env bash
# Rehydrates a devbox from the S3 backups (design/human/663.Backups.md):
#
#   tools/rehydrate.sh DIR [MOMENT]
#
# MOMENT is like 2026-10-26, 2026-10-26T18:00, or 2026-10-26T18:00:00Z (UTC); without one,
# the latest restore point. Where the backups are, and the keys to read them, come from
# GOOBER_BACKUP_ENV (default ~/.config/goober/backup.env), readable by you alone:
#
#   BUCKET=my-goober-backups
#   REGION=us-east-1
#   PREFIX=goober/                 # the host's backup.s3.prefix, if it has one
#   ENDPOINT=                      # for an S3-compatible service; empty for AWS
#   AWS_ACCESS_KEY_ID=AKIA...      # read-only access to the bucket is enough
#   AWS_SECRET_ACCESS_KEY=...
#   GOOBER_BACKUP_KEY=...          # from `goober-host backup key` on the server
#
# The devbox is a sandbox: run it with `just run DIR`; nothing it does leaves the machine.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${GOOBER_HOST_BIN:-$(dirname "$HERE")/build/dev/packages/host/goober-host}"
DIR="${1:?usage: tools/rehydrate.sh DIR [MOMENT]}"
AT="${2:-}"
ENV_FILE="${GOOBER_BACKUP_ENV:-$HOME/.config/goober/backup.env}"
[[ -f "$ENV_FILE" ]] || { sed -n '2,20p' "$0" >&2; echo "no $ENV_FILE" >&2; exit 2; }
[[ "$(stat -c %a "$ENV_FILE")" =~ ^[0-7]00$ ]] || { echo "$ENV_FILE holds secrets: chmod 600 it" >&2; exit 2; }
set -a
# shellcheck disable=SC1090
source "$ENV_FILE"
set +a
args=(rehydrate "$DIR" --bucket "${BUCKET:?BUCKET is required}" --region "${REGION:-us-east-1}")
[[ -n "${PREFIX:-}" ]] && args+=(--prefix "$PREFIX")
[[ -n "${ENDPOINT:-}" ]] && args+=(--endpoint "$ENDPOINT")
[[ -n "$AT" ]] && args+=(--at "$AT")
exec "$BIN" "${args[@]}"

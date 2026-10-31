#!/usr/bin/env bash
# Reads source projects' history back from the S3 backups (design/machine/020.Source.md § 11):
#
#   tools/source-mirror.sh DIR [PROJECT]
#
# Every revision's patch and every deployed revision's snapshot, unsealed into
# DIR/PROJECT/patches/ and DIR/PROJECT/snapshots/; a second run fetches only what's new. Where
# the backups are, and the keys to read them, come from GOOBER_BACKUP_ENV (default
# ~/.config/goober/backup.env), the same file tools/rehydrate.sh reads.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${GOOBER_HOST_BIN:-$(dirname "$HERE")/build/dev/packages/host/goober-host}"
DIR="${1:?usage: tools/source-mirror.sh DIR [PROJECT]}"
PROJECT="${2:-}"
ENV_FILE="${GOOBER_BACKUP_ENV:-$HOME/.config/goober/backup.env}"
[[ -f "$ENV_FILE" ]] || { echo "no $ENV_FILE: see tools/rehydrate.sh for what it holds" >&2; exit 2; }
[[ "$(stat -c %a "$ENV_FILE")" =~ ^[0-7]00$ ]] || { echo "$ENV_FILE holds secrets: chmod 600 it" >&2; exit 2; }
set -a
# shellcheck disable=SC1090
source "$ENV_FILE"
set +a
args=(source-mirror "$DIR" --bucket "${BUCKET:?BUCKET is required}" --region "${REGION:-us-east-1}")
[[ -n "${PREFIX:-}" ]] && args+=(--prefix "$PREFIX")
[[ -n "${ENDPOINT:-}" ]] && args+=(--endpoint "$ENDPOINT")
[[ -n "$PROJECT" ]] && args+=(--project "$PROJECT")
exec "$BIN" "${args[@]}"

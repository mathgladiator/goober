#!/usr/bin/env bash
# Vendors the Rust crates boxes may use, for builds on a host (design/machine/020.Source.md §4):
# builds run with no network, so a crate is usable only if it's in this directory.
#
#   tools/vendor.sh DEST
#
# Then set "source": {"vendor": "DEST"} in the host's config. The set is serde and
# serde_json (with derive); add a crate to CRATES below to offer more. Uses the network when it
# can, else cargo's local cache.
set -euo pipefail
DEST="$(realpath -m "${1:?usage: tools/vendor.sh DEST}")"
CRATES='serde_json = "1"
serde = { version = "1", features = ["derive"] }'
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/src"
touch "$WORK/src/lib.rs"
printf '[package]\nname = "goober-vendor-set"\nversion = "0.1.0"\nedition = "2024"\n[lib]\npath = "src/lib.rs"\n[dependencies]\n%s\n' \
  "$CRATES" >"$WORK/Cargo.toml"
rm -rf "$DEST.new"
(cd "$WORK" && { cargo vendor --versioned-dirs "$DEST.new" >/dev/null 2>&1 || cargo vendor --offline --versioned-dirs "$DEST.new" >/dev/null; })
rm -rf "$DEST"
mv "$DEST.new" "$DEST"
echo "vendored $(ls "$DEST" | wc -l) crates into $DEST"

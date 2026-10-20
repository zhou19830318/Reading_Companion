#!/usr/bin/env bash
# Builds a self-contained Voice Relay install package:
#   dist/voice-relay-<TS>.tar.gz
# containing relay/, dict-data/ (offline dictionary cards), deploy/
# (install.sh + systemd/nginx units), tools/, requirements.txt and README.md.
#
# On the target server:
#   tar xzf voice-relay-<TS>.tar.gz -C /tmp
#   sudo bash /tmp/voice-relay-<TS>/deploy/install.sh --source /tmp/voice-relay-<TS>
#
# install.sh prints the Relay URL / Device ID / Token for the reader's web UI
# at the end; on an interactive first run it also prompts for the DashScope
# API key (blank = mock mode).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT_DIR=${1:-"$ROOT/dist"}
TS=$(date +%Y%m%d-%H%M%S)
PKG_NAME=voice-relay-$TS

[[ -f "$ROOT/requirements.txt" && -d "$ROOT/relay" && -d "$ROOT/deploy" ]] || {
    echo "run from a full relay checkout (needs relay/, deploy/, requirements.txt)" >&2
    exit 1
}
[[ -f "$ROOT/dict-data/chars.json" ]] || {
    echo "missing dict-data/chars.json — see README 'Dictionary data'" >&2
    exit 1
}

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$OUT_DIR" "$STAGE/$PKG_NAME"
cp -r "$ROOT/relay" "$STAGE/$PKG_NAME/"
cp -r "$ROOT/dict-data" "$STAGE/$PKG_NAME/"
cp -r "$ROOT/deploy" "$STAGE/$PKG_NAME/"
cp -r "$ROOT/tools" "$STAGE/$PKG_NAME/"
cp "$ROOT/requirements.txt" "$STAGE/$PKG_NAME/"
[[ -f "$ROOT/README.md" ]] && cp "$ROOT/README.md" "$STAGE/$PKG_NAME/"
[[ -f "$ROOT/DEPLOY.md" ]] && cp "$ROOT/DEPLOY.md" "$STAGE/$PKG_NAME/"
find "$STAGE" \( -name __pycache__ -o -name '*.pyc' -o -name dist \) -prune -exec rm -rf {} + 2>/dev/null || true

tar czf "$OUT_DIR/$PKG_NAME.tar.gz" -C "$STAGE" "$PKG_NAME"
echo "package: $OUT_DIR/$PKG_NAME.tar.gz ($(du -h "$OUT_DIR/$PKG_NAME.tar.gz" | cut -f1))"
echo "install: tar xzf $PKG_NAME.tar.gz -C /tmp && sudo bash /tmp/$PKG_NAME/deploy/install.sh --source /tmp/$PKG_NAME"

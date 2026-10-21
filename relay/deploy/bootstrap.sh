#!/usr/bin/env bash
# One-command Voice Relay installer for Ubuntu / Debian.
#
#   curl -fsSL https://raw.githubusercontent.com/zhou19830318/Reading_Companion/main/relay/deploy/bootstrap.sh \
#     | sudo bash -s -- --dashscope-key sk-...
#
# Everything after `--` is forwarded to deploy/install.sh (--mode dev|prod,
# --domain, --public-url, --device-id, --dashscope-key, --asr-model, ...).
# With no DashScope key the relay installs in mock mode and can be upgraded
# later by re-running with --dashscope-key.
#
# Air-gapped hosts (no GitHub access):
#   sudo bash bootstrap.sh --tarball /path/to/voice-relay-<ts>.tar.gz [install.sh flags]
#   sudo bash bootstrap.sh --source /path/to/relay-checkout    [install.sh flags]
# A relay package tarball (make-package.sh) and a full repository checkout are
# both accepted; the script locates deploy/install.sh inside either layout.
set -euo pipefail

REPO_SLUG=${RELAY_REPO_SLUG:-zhou19830318/Reading_Companion}
REF=${RELAY_REF:-main}
SOURCE=""
TARBALL=""
FORWARD=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source)  SOURCE="${2:?--source needs a path}"; shift 2 ;;
        --tarball) TARBALL="${2:?--tarball needs a path or URL}"; shift 2 ;;
        --repo)    REPO_SLUG="${2:?--repo needs owner/name}"; shift 2 ;;
        --ref)     REF="${2:?--ref needs a branch or tag}"; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) FORWARD+=("$1"); shift ;;
    esac
done

if [[ $EUID -ne 0 ]]; then
    echo "run with sudo" >&2
    exit 1
fi

download() { # url dest
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 --connect-timeout 15 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$2" "$1"
    else
        echo "need curl or wget" >&2
        exit 1
    fi
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if [[ -z "$SOURCE" ]]; then
    if [[ -n "$TARBALL" ]]; then
        if [[ "$TARBALL" == http://* || "$TARBALL" == https://* ]]; then
            echo "== download $TARBALL"
            download "$TARBALL" "$TMP/pkg.tar.gz"
        else
            cp "$TARBALL" "$TMP/pkg.tar.gz"
        fi
    else
        url="https://codeload.github.com/$REPO_SLUG/tar.gz/refs/heads/$REF"
        echo "== download $url"
        download "$url" "$TMP/pkg.tar.gz"
    fi
    tar xzf "$TMP/pkg.tar.gz" -C "$TMP"
    INSTALL_SH=$(find "$TMP" -maxdepth 5 -type f -path '*/deploy/install.sh' | head -n 1)
    [[ -n "$INSTALL_SH" ]] || { echo "deploy/install.sh not found in package" >&2; exit 1; }
    SOURCE=$(cd "$(dirname "$INSTALL_SH")/.." && pwd)
fi

[[ -f "$SOURCE/requirements.txt" && -d "$SOURCE/relay" && -f "$SOURCE/deploy/install.sh" ]] || {
    echo "--source must contain relay/, deploy/ and requirements.txt" >&2
    exit 1
}

echo "== source: $SOURCE"
exec bash "$SOURCE/deploy/install.sh" --source "$SOURCE" "${FORWARD[@]}"

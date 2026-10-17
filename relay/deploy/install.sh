#!/usr/bin/env bash
# Reading Companion Voice Relay installer / upgrader (idempotent).
#
#   sudo bash install.sh --source /path/to/relay-src [--mode dev|prod] [--domain voice.example.com]
#        [--dashscope-key sk-...] [--device-id reading-companion-001]
#        [--asr-model paraformer-realtime-v2] [--llm-model qwen-flash]
#        [--dashscope-region cn-beijing] [--public-url ws://host:8080/api/v1/voice/stream]
#
# Creates a new timestamped release under
#   /opt/reading-companion-relay/releases/<TS>/
# and atomically switches /opt/reading-companion-relay/current to it, so a
# rollback is just: ln -sfn releases/<old-TS> current && systemctl restart ...
#
# --source must contain: relay/ (the Python package) and requirements.txt.
#
# First run without --dashscope-key on a terminal prompts for the key (blank
# keeps mock mode). Passing --dashscope-key on any run configures or rotates
# the DashScope providers without touching anything else. The end of the run
# prints the Relay URL / Device ID / Token to paste into the reader's web UI
# (Settings > Voice Relay).
set -euo pipefail

RELAY_ROOT=/opt/reading-companion-relay
ENV_DIR=/etc/reading-companion
ENV_FILE=$ENV_DIR/voice-relay.env
SERVICE_NAME=reading-companion-relay
NGINX_SITE=/etc/nginx/sites-available/reading-companion-relay.conf
KEEP_RELEASES=5

MODE=dev
DOMAIN=""
SOURCE=""
DASHSCOPE_KEY=""
DASHSCOPE_REGION="cn-beijing"
ASR_MODEL="paraformer-realtime-v2"
LLM_MODEL="qwen-flash"
DEVICE_ID="reading-companion-001"
PUBLIC_URL=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source) SOURCE="$2"; shift 2 ;;
        --mode)   MODE="$2"; shift 2 ;;
        --domain) DOMAIN="$2"; shift 2 ;;
        --dashscope-key)    DASHSCOPE_KEY="$2"; shift 2 ;;
        --dashscope-region) DASHSCOPE_REGION="$2"; shift 2 ;;
        --asr-model)        ASR_MODEL="$2"; shift 2 ;;
        --llm-model)        LLM_MODEL="$2"; shift 2 ;;
        --device-id)        DEVICE_ID="$2"; shift 2 ;;
        --public-url)       PUBLIC_URL="$2"; shift 2 ;;
        -h|--help) grep '^#' "$0" | head -24; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ $EUID -eq 0 ]] || { echo "run with sudo" >&2; exit 1; }
[[ -n "$SOURCE" && -d "$SOURCE/relay" && -f "$SOURCE/requirements.txt" ]] || {
    echo "--source must contain relay/ and requirements.txt" >&2; exit 1; }
[[ "$MODE" == "dev" || "$MODE" == "prod" ]] || { echo "--mode dev|prod" >&2; exit 1; }
if [[ "$MODE" == "prod" && -z "$DOMAIN" ]]; then
    echo "--mode prod requires --domain" >&2; exit 1
fi

TS=$(date +%Y%m%d-%H%M%S)
RELEASE="$RELAY_ROOT/releases/$TS"
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

section() { printf '\n== %s ==\n' "$*"; }

section "system packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
for pkg in python3 python3-venv python3-pip nginx curl jq ca-certificates openssl; do
    dpkg -s "$pkg" >/dev/null 2>&1 || apt-get install -y -qq "$pkg"
done

section "voice-relay user and directories"
id -u voice-relay >/dev/null 2>&1 || \
    useradd --system --home "$RELAY_ROOT" --shell /usr/sbin/nologin voice-relay
mkdir -p "$RELAY_ROOT"/{releases,logs,run,deploy} "$ENV_DIR" "$RELEASE"
chown -R voice-relay:voice-relay "$RELAY_ROOT"

section "release $TS"
cp -r "$SOURCE/relay" "$RELEASE/relay"
cp "$SOURCE/requirements.txt" "$RELEASE/"
find "$RELEASE" -name __pycache__ -type d -prune -exec rm -rf {} + 2>/dev/null || true
find "$RELEASE" -name '*.pyc' -delete 2>/dev/null || true

section "python venv"
python3 -m venv "$RELEASE/.venv"
"$RELEASE/.venv/bin/pip" install -q --upgrade pip wheel
"$RELEASE/.venv/bin/pip" install -q -r "$RELEASE/requirements.txt"
"$RELEASE/.venv/bin/pip" freeze > "$RELEASE/pip-freeze.txt"
chown -R voice-relay:voice-relay "$RELEASE"

# First-run terminal prompt: the DashScope key is the only required secret.
# Blank keeps mock mode. Skipped when stdin is not a terminal, so scripted
# deploys (and re-runs) stay non-interactive.
if [[ ! -f "$ENV_FILE" && -z "$DASHSCOPE_KEY" && -t 0 ]]; then
    printf '\nDashScope (Alibaba Cloud Model Studio) API key for real speech + LLM.\n'
    printf 'Leave blank to install in mock mode (canned answers, for wiring tests).\n'
    read -r -p "DASHSCOPE_API_KEY: " DASHSCOPE_KEY || true
    read -r -p "Device ID [$DEVICE_ID]: " _device_answer || true
    DEVICE_ID=${_device_answer:-$DEVICE_ID}
fi
[[ -n "$DASHSCOPE_KEY" ]] && echo "DashScope key provided: configuring real providers"

upsert_env() { # key value -- idempotent set (API keys and models are plain tokens)
    local key="$1" value="$2"
    if grep -q "^${key}=" "$ENV_FILE"; then
        sed -i "s|^${key}=.*|${key}=${value}|" "$ENV_FILE"
    else
        printf '%s=%s\n' "$key" "$value" >> "$ENV_FILE"
    fi
}

section "environment file"
if [[ -f "$ENV_FILE" ]]; then
    echo "keeping existing $ENV_FILE"
    # Track the release id for /healthz without touching secrets.
    sed -i "s/^RELAY_VERSION=.*/RELAY_VERSION=$TS/" "$ENV_FILE"
    # Backfill keys added after the first install (never clobber operator values).
    for kv in MAX_WS_CONNECTIONS=32 MAX_SESSIONS_PER_DEVICE=1; do
        key=${kv%%=*}
        grep -q "^${key}=" "$ENV_FILE" || echo "$kv" >> "$ENV_FILE"
    done
    if [[ -n "$DASHSCOPE_KEY" ]]; then
        upsert_env ASR_PROVIDER paraformer
        upsert_env ASR_MODEL "$ASR_MODEL"
        upsert_env LLM_PROVIDER dashscope-openai-compatible
        upsert_env LLM_MODEL "$LLM_MODEL"
        upsert_env LLM_DISABLE_THINKING true
        upsert_env DASHSCOPE_API_KEY "$DASHSCOPE_KEY"
        upsert_env DASHSCOPE_REGION "$DASHSCOPE_REGION"
        echo "updated DashScope providers in $ENV_FILE (key rotated, models refreshed)"
    fi
else
    TOKEN=$(openssl rand -hex 24)
    ADMIN=$(openssl rand -hex 24)
    if [[ -n "$DASHSCOPE_KEY" ]]; then
        provider_block="ASR_PROVIDER=paraformer
ASR_MODEL=$ASR_MODEL
LLM_PROVIDER=dashscope-openai-compatible
LLM_MODEL=$LLM_MODEL
LLM_DISABLE_THINKING=true
DASHSCOPE_API_KEY=$DASHSCOPE_KEY
DASHSCOPE_REGION=$DASHSCOPE_REGION"
    else
        provider_block="ASR_PROVIDER=mock
LLM_PROVIDER=mock
# Mock mode: canned replies only. Re-run with --dashscope-key sk-... (or edit
# this file, see deploy/voice-relay.env.example) to switch to real providers."
    fi
    cat > "$ENV_FILE" <<EOF
# Managed by install.sh. Secrets ONLY live here (root:voice-relay 640).
RELAY_ENV=development
RELAY_BIND=127.0.0.1
RELAY_PORT=8000
RELAY_LOG_LEVEL=INFO
RELAY_VERSION=$TS
RELAY_DEVICE_TOKEN=$TOKEN
RELAY_ADMIN_TOKEN=$ADMIN
$provider_block
MAX_AUDIO_SECONDS=30
MAX_LLM_OUTPUT_TOKENS=256
MAX_REPLY_CHARS=500
SESSION_IDLE_TIMEOUT_SECONDS=60
# Sessions may run concurrently; keep within the DashScope account quota.
MAX_CONCURRENT_SESSIONS=4
# Multiple onepage devices: cap open sockets and sessions per device.
MAX_WS_CONNECTIONS=32
MAX_SESSIONS_PER_DEVICE=1
EOF
    echo "created $ENV_FILE (device token generated)"
fi
chown root:voice-relay "$ENV_FILE"
chmod 640 "$ENV_FILE"

section "systemd service"
ln -sfn "$RELEASE" "$RELAY_ROOT/current"
install -m 644 "$SCRIPT_DIR/reading-companion-relay.service" /etc/systemd/system/$SERVICE_NAME.service
systemctl daemon-reload
systemctl enable $SERVICE_NAME >/dev/null
systemctl restart $SERVICE_NAME

section "nginx ($MODE)"
if [[ "$MODE" == "dev" ]]; then
    install -m 644 "$SCRIPT_DIR/nginx-voice-relay-dev.conf" "$NGINX_SITE"
    [[ -e /etc/nginx/sites-enabled/reading-companion-relay.conf ]] || \
        ln -sfn "$NGINX_SITE" /etc/nginx/sites-enabled/reading-companion-relay.conf
else
    sed "s/voice\.example\.com/$DOMAIN/g" "$SCRIPT_DIR/nginx-voice-relay-prod.conf" > "$NGINX_SITE"
    [[ -e /etc/nginx/sites-enabled/reading-companion-relay.conf ]] || \
        ln -sfn "$NGINX_SITE" /etc/nginx/sites-enabled/reading-companion-relay.conf
fi
nginx -t
systemctl enable nginx >/dev/null 2>&1 || true
systemctl reload nginx

section "smoke test"
for i in $(seq 1 15); do
    if curl -fsS http://127.0.0.1:8000/healthz >/dev/null 2>&1; then break; fi
    sleep 1
done
echo "-- direct (127.0.0.1:8000)"
curl -fsS http://127.0.0.1:8000/healthz | jq .
curl -fsS http://127.0.0.1:8000/readyz | jq .
if [[ "$MODE" == "dev" ]]; then
    echo "-- via nginx (8080)"
    curl -fsS http://127.0.0.1:8080/healthz | jq .
fi

section "prune old releases (keep $KEEP_RELEASES)"
mapfile -t OLD < <(ls -1dt "$RELAY_ROOT"/releases/*/ 2>/dev/null | tail -n +$((KEEP_RELEASES + 1)) || true)
for dir in "${OLD[@]:-}"; do
    [[ -n "$dir" ]] && rm -rf "$dir"
done

section "configure the reader (web UI > Settings > Voice Relay)"
if [[ -n "$PUBLIC_URL" ]]; then
    DEVICE_URL="$PUBLIC_URL"
elif [[ "$MODE" == "prod" ]]; then
    DEVICE_URL="wss://$DOMAIN/api/v1/voice/stream"
else
    DETECTED_IP=$(curl -fsS --max-time 5 https://cloudflare.com/cdn-cgi/trace 2>/dev/null | awk -F= '/^ip=/{print $2}') || true
    [[ -z "$DETECTED_IP" ]] && DETECTED_IP=$(hostname -I 2>/dev/null | awk '{print $1}')
    [[ -z "$DETECTED_IP" ]] && DETECTED_IP="<server-ip>"
    DEVICE_URL="ws://$DETECTED_IP:8080/api/v1/voice/stream"
fi
TOKEN_VALUE=$(grep '^RELAY_DEVICE_TOKEN=' "$ENV_FILE" | cut -d= -f2-)
printf '  1. Keep the reader on Wi-Fi, open its web UI (http://<reader-ip>/settings)\n'
printf '  2. Voice Relay card -> paste:\n'
printf '       Relay URL:  %s\n' "$DEVICE_URL"
printf '       Device ID:  %s\n' "$DEVICE_ID"
printf '       Token:      %s\n' "$TOKEN_VALUE"
printf '  3. Save. The device-side Voice Relay toggle is ON by default.\n'
printf '  MiMo speech key and OpenClaw can stay blank: the relay replaces both.\n'
if ! grep -q '^DASHSCOPE_API_KEY=sk-' "$ENV_FILE"; then
    printf '\n  NOTE: mock mode is active -- voice turns return canned text until a\n'
    printf '  DashScope key is set. Re-run this script with --dashscope-key sk-...\n'
fi
if [[ "$MODE" == "dev" ]]; then
    printf '  (dev mode: also make sure the cloud security group opens TCP/8080)\n'
fi

section "done"
echo "release:  $TS"
echo "service:  systemctl status $SERVICE_NAME --no-pager"
echo "logs:     journalctl -u $SERVICE_NAME -f"
echo "rollback: ln -sfn $RELAY_ROOT/releases/<old-TS> $RELAY_ROOT/current && systemctl restart $SERVICE_NAME"
echo "token:    sudo grep RELAY_DEVICE_TOKEN $ENV_FILE"

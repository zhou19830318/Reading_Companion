# Reading Companion Voice Relay

FastAPI service in front of DashScope (Alibaba Cloud Model Studio) realtime
ASR and streaming LLM. The ESP32-C61 onepage devices never see the cloud API
key or endpoint; they speak a small JSON + PCM protocol over one WebSocket to
this relay.

```
device ──WS /api/v1/voice/stream──▶ relay ── WSS ──▶ DashScope ASR (paraformer-realtime-v2)
                                      │
                                      └── HTTPS SSE ─▶ DashScope LLM (qwen-flash, OpenAI-compatible)
```

## Protocol (v1)

- 16 kHz mono PCM s16le, 40 ms frames (1280 bytes), max 30 s per utterance.
- Device → relay: `session.start` → `audio.start` → binary frames →
  `audio.end` (or `session.cancel`).
- Relay → device: `session.started`, `asr.partial`, `asr.final`, `llm.start`,
  `llm.delta`, `llm.final`, `session.done`, `error`
  (codes: `BAD_PROTOCOL`, `UNAUTHORIZED`, `SERVER_OVERLOADED`,
  `ASR_CONNECTION_ERROR`, `ASR_AUTH_ERROR`, `ASR_RATE_LIMITED`, ...).
- In-band errors do not close the socket; protocol/auth violations do.
- Handshake headers: `Authorization: Bearer <token>`, `X-Device-Id`,
  `X-Voice-Protocol: 1`.

Runnable client without hardware: `python tools/test_client.py --pcm speech.wav`.

## Auth (docs section 17.5)

`RELAY_DEVICE_TOKEN` is the shared token; `RELAY_DEVICE_TOKENS` can pin
per-device tokens as `deviceId:token,...` (a per-device entry wins over the
shared token). `ALLOWED_DEVICE_IDS` optionally allow-lists device ids. An
empty configuration refuses to run open. `RELAY_ADMIN_TOKEN` enables the
admin-only `POST /api/v1/chat` text debug endpoint.

## Multiple onepage devices (multi-device design)

Several onepage devices connect to the same relay; capacity is enforced in
three layers so one misbehaving device cannot starve the others:

| Layer | Setting (env) | Default | Exceeded → |
|---|---|---|---|
| Open device sockets | `MAX_WS_CONNECTIONS` | 32 | new socket accepted then closed with WS code 1013 (try again later); `ws_rejected_total{reason="connections"}` |
| Concurrent voice sessions (all devices) | `MAX_CONCURRENT_SESSIONS` | 4 | in-band `error: SERVER_OVERLOADED`; `ws_rejected_total{reason="sessions"}` |
| Concurrent sessions per device | `MAX_SESSIONS_PER_DEVICE` | 1 | in-band `error: SERVER_OVERLOADED`; `ws_rejected_total{reason="device_sessions"}` |

Notes designed in for multi-device operation:

- `MAX_CONCURRENT_SESSIONS` is the DashScope quota governor: every active
  session holds one upstream ASR WebSocket and one LLM HTTPS request. Keep it
  at or below the account's concurrency quota for the ASR model; a value of 4
  is load-tested (4 clients × 5 rounds, zero failures).
- The device identity used for the per-device cap and all logs is the
  authenticated `X-Device-Id` header, not the `deviceId` inside
  `session.start` (a device cannot dodge its own cap by relabelling).
- Idle sockets are reaped after `SESSION_IDLE_TIMEOUT_SECONDS` (60 s) when no
  session is running, so abandoned connections do not count against the caps.
- `SERVER_OVERLOADED` is retryable: the device should surface a short
  "service busy" and fall back to the legacy MiMo STT + OpenClaw path.
- The relay is a single process on purpose: sessions, caps and metrics are
  in-memory and consistent. If horizontal scaling ever becomes necessary,
  keep affinity (one device → one instance) with sticky routing in nginx,
  or move counters/caps to Redis; do not run multiple uvicorn workers behind
  a plain round-robin — the caps would be per-worker and aggregate metrics
  would double-count.

Load test for multi-device verification:

```bash
RELAY_DEVICE_TOKEN=... python tools/load_test.py --pcm speech.pcm \
    --clients 4 --rounds 5            # 4 devices, one reused socket each
```

## Providers

`ASR_PROVIDER`/`LLM_PROVIDER` are `mock` (development, no upstream) or the
real DashScope adapters. Real credentials only ever live in
`/etc/reading-companion/voice-relay.env` (`DASHSCOPE_API_KEY`,
`DASHSCOPE_WORKSPACE_ID`, `DASHSCOPE_REGION`); see
`deploy/voice-relay.env.example`. The startup log prints the full upstream
contract per provider (the "contract gate"); verify it after every change:

```bash
journalctl -u reading-companion-relay --since "5 min ago" | grep contracts
curl -s localhost:8000/readyz      # {"status":"ready","asr":"configured",...}
```

## Deploy and rollback

```bash
# package: build a self-contained tarball under dist/
bash tools/make-package.sh
# install: unpack on the server and run the installer
tar xzf voice-relay-<TS>.tar.gz -C /tmp
sudo bash /tmp/voice-relay-<TS>/deploy/install.sh --source /tmp/voice-relay-<TS> --mode dev
# or from a source checkout directly
sudo bash deploy/install.sh --source /tmp/relay-src --mode dev
# rollback (<old-TS> from /opt/reading-companion-relay/releases/)
ln -sfn /opt/reading-companion-relay/releases/<old-TS> /opt/reading-companion-relay/current
systemctl restart reading-companion-relay
```

`install.sh` is idempotent: new timestamped release, environment file kept
(keys added later are backfilled), nginx site managed, smoke test, old
releases pruned (keep 5). Modes: `dev` (ws:// on :8080 via nginx) and `prod`
(wss:// on 443 once the cloud console opens the port and a domain exists).

DashScope setup: on an interactive first run the installer prompts for the
API key (blank = mock mode); pass `--dashscope-key sk-...` on any run to
configure or rotate it non-interactively (`--asr-model`, `--llm-model`,
`--dashscope-region`, `--device-id`, `--public-url` are optional overrides).
The end of the run prints the Relay URL / Device ID / Token to paste into the
reader's web UI (Settings > Voice Relay). The reader needs nothing else: with
the relay configured, the device-side MiMo key and OpenClaw become optional.

## Observability

- `GET /metrics` (internal only): `voice_sessions_total{result}`,
  `voice_sessions_active`, `ws_connections_active`, `ws_rejected_total{reason}`,
  `voice_latency_ms{stage}` (`asrFirstPartial`, `asrFinal`, `llmFirstToken`,
  `llmFinal`, `visibleTotal`), `asr_requests_total`, `llm_requests_total`,
  `websocket_disconnects_total{reason}`, `relay_process_rss_bytes`.
- Structured JSON logs, one event per session state change; the redaction
  filter guarantees an API key can never be logged (even by a careless call).

## Tests

```bash
pip install -r requirements-dev.txt
python -m pytest -q          # protocol, session, auth, providers, capacity, mock e2e
```

Real-credential contract check (no hardware needed):

```bash
python tools/test_client.py --pcm speech-16k.pcm --realtime
```

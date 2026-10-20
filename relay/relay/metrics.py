"""Prometheus metrics (docs section 17.5.5).

No label ever carries user text, audio, tokens or request ids: cardinality
stays bounded and /metrics leaks nothing sensitive.
"""

from __future__ import annotations

import os

from prometheus_client import Counter, Gauge, Histogram

voice_sessions_total = Counter(
    "voice_sessions_total",
    "Finished voice sessions by result",
    ["result"],
)
voice_sessions_active = Gauge(
    "voice_sessions_active",
    "Voice sessions currently active",
)
audio_frames_received_total = Counter(
    "audio_frames_received_total",
    "Binary audio frames received from devices",
)
audio_frames_dropped_total = Counter(
    "audio_frames_dropped_total",
    "Binary audio frames dropped (overflow/oversize/after end)",
)
asr_requests_total = Counter(
    "asr_requests_total",
    "ASR upstream requests by result",
    ["result"],
)
llm_requests_total = Counter(
    "llm_requests_total",
    "LLM upstream requests by result",
    ["result"],
)
t2i_requests_total = Counter(
    "t2i_requests_total",
    "Text-to-image generation requests by result",
    ["result"],
)
voice_latency_ms = Histogram(
    "voice_latency_ms",
    "Per-stage voice latency in milliseconds",
    ["stage"],
    buckets=(100, 200, 300, 500, 800, 1200, 2000, 3000, 5000, 10000, 30000),
)
upstream_reconnects_total = Counter(
    "upstream_reconnects_total",
    "Upstream reconnect/retry attempts",
    ["provider"],
)
websocket_disconnects_total = Counter(
    "websocket_disconnects_total",
    "Device WebSocket disconnects by reason",
    ["reason"],
)
ws_connections_active = Gauge(
    "ws_connections_active",
    "Device WebSocket connections currently open",
)
ws_rejected_total = Counter(
    "ws_rejected_total",
    "Connections or sessions rejected by capacity limits",
    ["reason"],
)
relay_process_rss_bytes = Gauge(
    "relay_process_rss_bytes",
    "Relay process resident set size",
)


def observe_rss() -> None:
    """Cheap RSS sampler; called by the metrics endpoint itself."""
    try:
        with open("/proc/self/statm", "rb") as fh:
            pages = int(fh.read().split()[1])
        relay_process_rss_bytes.set(pages * os.sysconf("SC_PAGE_SIZE"))
    except (OSError, ValueError, IndexError):
        pass

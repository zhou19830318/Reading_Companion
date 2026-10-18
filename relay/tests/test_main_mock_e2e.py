"""End-to-end tests through the real FastAPI app with mock providers."""

from __future__ import annotations

import json

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from relay.config import reload_settings
from relay.main import app
from relay.providers.base import LlmUsage

HEADERS = {
    "Authorization": "Bearer test-device-token",
    "X-Device-Id": "debug-client",
    "X-Voice-Protocol": "1",
}
ADMIN = {"Authorization": "Bearer test-admin-token"}

FRAME_40MS = b"\x01\x02" * 640  # 1280 bytes, 16 kHz mono s16le


@pytest.fixture()
def client():
    reload_settings()
    with TestClient(app) as c:
        yield c


def session_start(request_id: str = "req-e2e", mode: str = "relay") -> dict:
    return {
        "type": "session.start",
        "requestId": request_id,
        "deviceId": "debug-client",
        "audio": {"sampleRate": 16000, "channels": 1, "format": "pcm_s16le", "frameMs": 40},
        "mode": mode,
    }


def collect_until_terminal(ws, limit: int = 300) -> list[dict]:
    msgs: list[dict] = []
    for _ in range(limit):
        msg = ws.receive_json()
        msgs.append(msg)
        if msg["type"] in ("session.done", "error"):
            break
    return msgs


# ---------------------------------------------------------------- http surface


def test_healthz_and_readyz(client):
    assert client.get("/healthz").json()["status"] == "ok"
    ready = client.get("/readyz")
    assert ready.status_code == 200
    assert ready.json()["asr"] == "configured"
    assert ready.json()["llm"] == "configured"


def test_device_config_requires_valid_token(client):
    assert client.get("/api/v1/device/config").status_code == 401
    r = client.get("/api/v1/device/config", headers=HEADERS)
    assert r.status_code == 200
    body = r.json()
    assert body["protocolVersion"] == 1
    assert body["audio"] == {
        "sampleRate": 16000,
        "channels": 1,
        "format": "pcm_s16le",
        "frameMs": 40,
    }
    assert body["limits"]["maxAudioSeconds"] == 30


def test_metrics_endpoint(client):
    r = client.get("/metrics")
    assert r.status_code == 200
    assert b"voice_sessions_total" in r.content


# ------------------------------------------------------------------ websocket


def test_ws_happy_path_mock(client):
    with client.websocket_connect("/api/v1/voice/stream", headers=HEADERS) as ws:
        ws.send_json(session_start())
        started = ws.receive_json()
        assert started["type"] == "session.started"
        sid = started["sessionId"]

        ws.send_json({"type": "audio.start", "requestId": "req-e2e", "sessionId": sid})
        for _ in range(25):  # 1 s of audio
            ws.send_bytes(FRAME_40MS)
        ws.send_json({"type": "audio.end", "requestId": "req-e2e", "sessionId": sid})

        msgs = collect_until_terminal(ws)
        types = [m["type"] for m in msgs]
        assert "asr.partial" in types
        assert "asr.final" in types
        assert "llm.start" in types
        assert "llm.delta" in types
        assert types[-1] == "session.done"
        done = msgs[-1]
        assert done["requestId"] == "req-e2e"
        latency = done["latencyMs"]
        assert "asrFirstPartial" in latency
        assert "asrFinal" in latency
        assert "llmFirstToken" in latency

        # The connection survives completion: a second session on the same WS.
        ws.send_json(session_start(request_id="req-e2e-2"))
        started2 = ws.receive_json()
        assert started2["type"] == "session.started"
        assert started2["requestId"] == "req-e2e-2"


def test_ws_dictation_returns_transcript_without_llm(client):
    with client.websocket_connect("/api/v1/voice/stream", headers=HEADERS) as ws:
        ws.send_json(session_start(request_id="req-dict", mode="dictation"))
        started = ws.receive_json()
        assert started["type"] == "session.started"
        sid = started["sessionId"]

        ws.send_json({"type": "audio.start", "requestId": "req-dict", "sessionId": sid})
        for _ in range(25):  # 1 s of audio
            ws.send_bytes(FRAME_40MS)
        ws.send_json({"type": "audio.end", "requestId": "req-dict", "sessionId": sid})

        msgs = collect_until_terminal(ws)
        types = [m["type"] for m in msgs]
        assert "asr.final" in types
        assert "llm.start" not in types
        assert "llm.delta" not in types
        assert types[-1] == "session.done"
        assert msgs[-1]["requestId"] == "req-dict"


def test_ws_audio_before_session_start_is_rejected(client):
    with client.websocket_connect("/api/v1/voice/stream", headers=HEADERS) as ws:
        ws.send_json({"type": "audio.start", "requestId": "r", "sessionId": "s"})
        with pytest.raises(WebSocketDisconnect) as exc:
            for _ in range(5):
                ws.receive_json()
        assert exc.value.code == 4400


def test_ws_wrong_request_id_gets_bad_protocol_error(client):
    with client.websocket_connect("/api/v1/voice/stream", headers=HEADERS) as ws:
        ws.send_json(session_start())
        started = ws.receive_json()
        sid = started["sessionId"]
        ws.send_json({"type": "audio.start", "requestId": "some-other-id", "sessionId": sid})
        msg = ws.receive_json()
        assert msg["type"] == "error"
        assert msg["code"] == "BAD_PROTOCOL"


def test_ws_double_audio_end_gets_error_not_disconnect(client):
    with client.websocket_connect("/api/v1/voice/stream", headers=HEADERS) as ws:
        ws.send_json(session_start())
        started = ws.receive_json()
        sid = started["sessionId"]
        ws.send_json({"type": "audio.start", "requestId": "req-e2e", "sessionId": sid})
        ws.send_bytes(FRAME_40MS)
        ws.send_json({"type": "audio.end", "requestId": "req-e2e", "sessionId": sid})
        msgs = collect_until_terminal(ws)  # this session completes normally
        assert msgs[-1]["type"] == "session.done"
        # A second audio.end on the finished session is answered, not fatal.
        ws.send_json({"type": "audio.end", "requestId": "req-e2e", "sessionId": sid})
        msg = ws.receive_json()
        assert msg["type"] == "error"
        assert msg["code"] == "BAD_PROTOCOL"


def test_ws_asr_failure_surfaces_error_and_connection_survives(client, monkeypatch):
    monkeypatch.setenv("MOCK_ASR_FAIL_START", "true")
    reload_settings()
    with client.websocket_connect("/api/v1/voice/stream", headers=HEADERS) as ws:
        ws.send_json(session_start())
        assert ws.receive_json()["type"] == "session.started"
        ws.send_json({"type": "audio.start", "requestId": "req-e2e", "sessionId": ""})
        ws.send_bytes(FRAME_40MS)
        msgs = collect_until_terminal(ws)
        assert msgs[-1]["type"] == "error"
        assert msgs[-1]["code"] == "ASR_CONNECTION_ERROR"
        assert msgs[-1]["retryable"] is True


def test_ws_cancel_after_audio_start_is_quiet(client):
    with client.websocket_connect("/api/v1/voice/stream", headers=HEADERS) as ws:
        ws.send_json(session_start())
        started = ws.receive_json()
        sid = started["sessionId"]
        ws.send_json({"type": "audio.start", "requestId": "req-e2e", "sessionId": sid})
        ws.send_bytes(FRAME_40MS)
        ws.send_json({"type": "session.cancel", "requestId": "req-e2e", "sessionId": sid})
        # No error frame; a new session can start immediately.
        ws.send_json(session_start(request_id="req-next"))
        msg = ws.receive_json()
        assert msg["type"] == "session.started"
        assert msg["requestId"] == "req-next"


# ----------------------------------------------------------------- text chat


def test_chat_requires_valid_token(client):
    assert client.post("/api/v1/chat", json={"text": "hi"}).status_code == 401
    assert (
        client.post(
            "/api/v1/chat", json={"text": "hi"}, headers={"Authorization": "Bearer nope"}
        ).status_code
        == 401
    )


def test_chat_allows_device_token(client):
    r = client.post("/api/v1/chat", json={"text": "你好", "stream": False}, headers=HEADERS)
    assert r.status_code == 200
    assert r.json()["text"] == "今天适合阅读，室外天气晴朗。"


def test_chat_non_stream(client):
    r = client.post("/api/v1/chat", json={"text": "你好", "stream": False}, headers=ADMIN)
    assert r.status_code == 200
    body = r.json()
    assert body["text"] == "今天适合阅读，室外天气晴朗。"
    assert body["usage"]["outputTokens"] > 0


def test_chat_stream_sse(client):
    with client.stream("POST", "/api/v1/chat", json={"text": "你好"}, headers=ADMIN) as r:
        assert r.status_code == 200
        lines = [line for line in r.iter_lines() if line.startswith("data: ")]
    events = [json.loads(line[6:]) for line in lines]
    types = [e["type"] for e in events]
    assert types[0] == "llm.start"
    assert "llm.delta" in types
    assert types[-1] == "session.done"


class _CapturingProvider:
    """Records stream() kwargs so tests can assert mode-specific overrides."""

    def __init__(self):
        self.calls: list[dict] = []

    async def stream(self, prompt, history, max_tokens, on_delta, system=None):
        self.calls.append({"prompt": prompt, "max_tokens": max_tokens, "system": system})
        text = "【字】踌\n【拼音】chóu\n【部首笔画】足·14画\n【释义】1. 犹豫不前。\n【组词】踌躇\n【例句】他踌躇了一会儿。"
        await on_delta(text)
        return text, LlmUsage(input_tokens=2, output_tokens=10)

    async def close(self):
        return None


def test_chat_dict_mode_uses_dict_prompt_and_token_budget(client, monkeypatch):
    from relay.config import get_settings, reload_settings

    # The optional LLM card path (DICT_SOURCE=llm); the default is the offline
    # data, covered in test_dict_source.py.
    monkeypatch.setenv("DICT_SOURCE", "llm")
    settings = reload_settings()
    provider = _CapturingProvider()
    monkeypatch.setattr("relay.main.make_llm_provider", lambda settings: provider)
    r = client.post("/api/v1/chat", json={"text": "踌", "stream": False, "mode": "dict"}, headers=ADMIN)
    assert r.status_code == 200
    assert "【拼音】" in r.json()["text"]
    assert provider.calls == [
        {"prompt": "踌", "max_tokens": settings.dict_max_tokens, "system": settings.llm_dict_system_prompt}
    ]


def test_chat_default_mode_keeps_default_prompt(client, monkeypatch):
    provider = _CapturingProvider()
    monkeypatch.setattr("relay.main.make_llm_provider", lambda settings: provider)
    r = client.post("/api/v1/chat", json={"text": "你好", "stream": False}, headers=ADMIN)
    assert r.status_code == 200
    assert provider.calls[0]["system"] is None

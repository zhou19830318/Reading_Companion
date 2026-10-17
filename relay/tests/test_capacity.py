"""Multi-device capacity limits: per-device sessions, global sessions, sockets."""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from relay.config import reload_settings
from relay.main import app


def headers(device: str) -> dict:
    return {
        "Authorization": "Bearer test-device-token",
        "X-Device-Id": device,
        "X-Voice-Protocol": "1",
    }


def session_start(device: str, request_id: str) -> dict:
    return {
        "type": "session.start",
        "requestId": request_id,
        "deviceId": device,
        "audio": {"sampleRate": 16000, "channels": 1, "format": "pcm_s16le", "frameMs": 40},
        "mode": "relay",
    }


@pytest.fixture(autouse=True)
def _neutralise_allowlist(monkeypatch):
    monkeypatch.setenv("ALLOWED_DEVICE_IDS", "")


@pytest.fixture()
def client():
    reload_settings()
    with TestClient(app) as c:
        yield c


def test_second_session_from_same_device_is_rejected(client, monkeypatch):
    monkeypatch.setenv("MAX_CONCURRENT_SESSIONS", "4")
    monkeypatch.setenv("MAX_SESSIONS_PER_DEVICE", "1")
    reload_settings()
    with client.websocket_connect("/api/v1/voice/stream", headers=headers("dev-a")) as ws1:
        ws1.send_json(session_start("dev-a", "req-a1"))
        assert ws1.receive_json()["type"] == "session.started"
        with client.websocket_connect("/api/v1/voice/stream", headers=headers("dev-a")) as ws2:
            ws2.send_json(session_start("dev-a", "req-a2"))
            msg = ws2.receive_json()
            assert msg["type"] == "error"
            assert msg["code"] == "SERVER_OVERLOADED"
            assert msg["requestId"] == "req-a2"


def test_other_device_is_not_affected_by_device_cap(client, monkeypatch):
    monkeypatch.setenv("MAX_CONCURRENT_SESSIONS", "4")
    monkeypatch.setenv("MAX_SESSIONS_PER_DEVICE", "1")
    reload_settings()
    with client.websocket_connect("/api/v1/voice/stream", headers=headers("dev-a")) as ws1:
        ws1.send_json(session_start("dev-a", "req-a1"))
        assert ws1.receive_json()["type"] == "session.started"
        with client.websocket_connect("/api/v1/voice/stream", headers=headers("dev-b")) as ws2:
            ws2.send_json(session_start("dev-b", "req-b1"))
            assert ws2.receive_json()["type"] == "session.started"


def test_global_session_cap_rejects_second_device(client, monkeypatch):
    monkeypatch.setenv("MAX_CONCURRENT_SESSIONS", "1")
    monkeypatch.setenv("MAX_SESSIONS_PER_DEVICE", "2")
    reload_settings()
    with client.websocket_connect("/api/v1/voice/stream", headers=headers("dev-a")) as ws1:
        ws1.send_json(session_start("dev-a", "req-a1"))
        assert ws1.receive_json()["type"] == "session.started"
        with client.websocket_connect("/api/v1/voice/stream", headers=headers("dev-b")) as ws2:
            ws2.send_json(session_start("dev-b", "req-b1"))
            msg = ws2.receive_json()
            assert msg["type"] == "error"
            assert msg["code"] == "SERVER_OVERLOADED"


def test_message_device_id_cannot_dodge_the_device_cap(client, monkeypatch):
    monkeypatch.setenv("MAX_CONCURRENT_SESSIONS", "4")
    monkeypatch.setenv("MAX_SESSIONS_PER_DEVICE", "1")
    reload_settings()
    with client.websocket_connect("/api/v1/voice/stream", headers=headers("dev-a")) as ws1:
        ws1.send_json(session_start("dev-a", "req-a1"))
        assert ws1.receive_json()["type"] == "session.started"
        with client.websocket_connect("/api/v1/voice/stream", headers=headers("dev-a")) as ws2:
            # Same authenticated header identity, spoofed message deviceId.
            ws2.send_json(session_start("dev-b", "req-a2"))
            msg = ws2.receive_json()
            assert msg["type"] == "error"
            assert msg["code"] == "SERVER_OVERLOADED"


def test_connection_cap_closes_extra_sockets_with_1013(client, monkeypatch):
    monkeypatch.setenv("MAX_WS_CONNECTIONS", "1")
    reload_settings()
    with client.websocket_connect("/api/v1/voice/stream", headers=headers("dev-a")) as ws1:
        ws1.send_json(session_start("dev-a", "req-a1"))
        assert ws1.receive_json()["type"] == "session.started"
        with pytest.raises(WebSocketDisconnect) as exc:
            with client.websocket_connect("/api/v1/voice/stream", headers=headers("dev-b")) as ws2:
                ws2.receive_json()
        assert exc.value.code == 1013

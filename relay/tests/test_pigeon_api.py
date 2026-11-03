"""Pigeon API: device auth, presence, store-and-forward, ack, limits."""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from relay.config import reload_settings
from relay.main import app

A = {"Authorization": "Bearer token-a", "X-Device-Id": "dev-a"}
B = {"Authorization": "Bearer token-b", "X-Device-Id": "dev-b"}


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("RELAY_PIGEON_DIR", str(tmp_path / "pigeon"))
    monkeypatch.setenv("ALLOWED_DEVICE_IDS", "dev-a,dev-b")
    monkeypatch.setenv("RELAY_DEVICE_TOKENS", "dev-a:token-a,dev-b:token-b")
    monkeypatch.setenv("PIGEON_RATE_LIMIT_PER_MINUTE", "3")
    reload_settings()
    with TestClient(app) as c:
        yield c
    reload_settings()


def test_requires_device_token(client):
    assert client.post("/api/v1/pigeon/heartbeat", json={}).status_code == 401
    assert client.get("/api/v1/pigeon/inbox").status_code == 401
    assert client.post("/api/v1/pigeon/send", json={"to": "dev-b", "text": "x"}).status_code == 401
    assert client.post("/api/v1/pigeon/clear", params={"peer": "dev-b"}).status_code == 401


def test_token_map_rejects_unknown_device_id(client):
    # RELAY_DEVICE_TOKEN is still set by the test environment, but a configured
    # map is authoritative; an unknown id cannot borrow the shared token.
    headers = {"Authorization": "Bearer test-device-token", "X-Device-Id": "dev-z"}
    assert client.post("/api/v1/pigeon/heartbeat", headers=headers, json={}).status_code == 401


def test_heartbeat_logs_at_info_without_logrecord_conflict(client):
    # The test environment runs at WARNING; production runs at INFO and must
    # not 500 on a structured-log field that collides with LogRecord.
    import logging

    root = logging.getLogger()
    previous = root.level
    root.setLevel(logging.INFO)
    try:
        assert client.post("/api/v1/pigeon/heartbeat", headers=A, json={"name": "书房"}).status_code == 200
    finally:
        root.setLevel(previous)


def test_heartbeat_lists_peers_with_online_state(client):
    first = client.post("/api/v1/pigeon/heartbeat", headers=A, json={"name": "书房"})
    assert first.status_code == 200
    assert first.json()["self"]["name"] == "书房"

    client.post("/api/v1/pigeon/heartbeat", headers=B, json={"name": "客厅"})
    body = client.post("/api/v1/pigeon/heartbeat", headers=A, json={"name": "书房"}).json()
    devices = {d["id"]: d for d in body["devices"]}
    assert devices["dev-b"]["online"] is True
    assert devices["dev-b"]["name"] == "客厅"


def test_send_store_forward_ack_and_history(client):
    sent = client.post(
        "/api/v1/pigeon/send",
        headers=A,
        json={"to": "dev-b", "text": "今晚读哪本？", "clientEventId": "m1"},
    )
    assert sent.status_code == 200
    message = sent.json()["message"]
    assert message["from"] == "dev-a"
    assert message["to"] == "dev-b"
    assert message["duplicate"] is False

    inbox = client.get("/api/v1/pigeon/inbox", headers=B).json()
    assert [m["text"] for m in inbox["messages"]] == ["今晚读哪本？"]
    assert inbox["unread"] == 1

    acked = client.post("/api/v1/pigeon/ack", headers=B, json={"ids": [message["id"]]})
    assert acked.json()["acked"] == 1
    assert client.get("/api/v1/pigeon/inbox", headers=B).json()["unread"] == 0

    reply = client.post(
        "/api/v1/pigeon/send",
        headers=B,
        json={"to": "dev-a", "text": "在读《小王子》", "clientEventId": "m2"},
    )
    assert reply.status_code == 200
    history = client.get("/api/v1/pigeon/history", headers=A, params={"peer": "dev-b"}).json()
    assert [m["text"] for m in history["messages"]] == ["今晚读哪本？", "在读《小王子》"]


def test_clear_history_removes_the_conversation(client):
    client.post("/api/v1/pigeon/send", headers=A, json={"to": "dev-b", "text": "one", "clientEventId": "c1"})
    client.post("/api/v1/pigeon/send", headers=B, json={"to": "dev-a", "text": "two", "clientEventId": "c2"})
    assert len(client.get("/api/v1/pigeon/history", headers=A, params={"peer": "dev-b"}).json()["messages"]) == 2

    cleared = client.post("/api/v1/pigeon/clear", headers=A, params={"peer": "dev-b"})
    assert cleared.status_code == 200
    assert cleared.json() == {"ok": True, "deleted": 2}
    assert client.get("/api/v1/pigeon/history", headers=A, params={"peer": "dev-b"}).json()["messages"] == []
    assert client.get("/api/v1/pigeon/history", headers=B, params={"peer": "dev-a"}).json()["messages"] == []

    unknown = client.post("/api/v1/pigeon/clear", headers=A, params={"peer": "dev-z"})
    assert unknown.status_code == 404
    assert unknown.json()["error"] == "unknown_peer"


def test_send_retry_is_idempotent(client):
    payload = {"to": "dev-b", "text": "重试", "clientEventId": "retry-1"}
    first = client.post("/api/v1/pigeon/send", headers=A, json=payload).json()
    again = client.post("/api/v1/pigeon/send", headers=A, json=payload).json()
    assert again["duplicate"] is True
    assert again["message"]["id"] == first["message"]["id"]
    assert len(client.get("/api/v1/pigeon/inbox", headers=B).json()["messages"]) == 1


def test_send_rejects_unknown_recipient_and_oversize_text(client):
    unknown = client.post(
        "/api/v1/pigeon/send",
        headers=A,
        json={"to": "dev-z", "text": "hi", "clientEventId": "x1"},
    )
    assert unknown.status_code == 404
    assert unknown.json()["error"] == "unknown_recipient"

    oversize = client.post(
        "/api/v1/pigeon/send",
        headers=A,
        json={"to": "dev-b", "text": "x" * 513, "clientEventId": "x2"},
    )
    assert oversize.status_code == 400
    assert oversize.json()["error"] == "text_too_long"


def test_send_rate_limit(client):
    for i in range(3):
        response = client.post(
            "/api/v1/pigeon/send",
            headers=A,
            json={"to": "dev-b", "text": f"m{i}", "clientEventId": f"r{i}"},
        )
        assert response.status_code == 200
    limited = client.post(
        "/api/v1/pigeon/send",
        headers=A,
        json={"to": "dev-b", "text": "m3", "clientEventId": "r3"},
    )
    assert limited.status_code == 429
    assert limited.json()["error"] == "rate_limited"


def test_registered_peer_stays_listed_when_offline_and_receives_later(client):
    client.post("/api/v1/pigeon/heartbeat", headers=A, json={"name": "书房"})
    client.post("/api/v1/pigeon/heartbeat", headers=B, json={"name": "客厅"})

    # Expire only dev-b's presence entry; the durable registry must keep it.
    client.app.state.pigeon_presence._last_seen["dev-b"] = -1000.0
    body = client.post("/api/v1/pigeon/heartbeat", headers=A, json={"name": "书房"}).json()
    peer = next(d for d in body["devices"] if d["id"] == "dev-b")
    assert peer["online"] is False
    assert peer["name"] == "客厅"

    sent = client.post(
        "/api/v1/pigeon/send",
        headers=A,
        json={"to": "dev-b", "text": "离线也能收", "clientEventId": "offline-1"},
    )
    assert sent.status_code == 200

    # dev-b comes back: the stored message is in its inbox and history.
    client.post("/api/v1/pigeon/heartbeat", headers=B, json={"name": "客厅"})
    inbox = client.get("/api/v1/pigeon/inbox", headers=B).json()
    assert [m["text"] for m in inbox["messages"]] == ["离线也能收"]
    history = client.get("/api/v1/pigeon/history", headers=B, params={"peer": "dev-a"}).json()
    assert [m["text"] for m in history["messages"]] == ["离线也能收"]


@pytest.fixture()
def registry_client(tmp_path, monkeypatch):
    monkeypatch.setenv("RELAY_PIGEON_DIR", str(tmp_path / "pigeon"))
    monkeypatch.setenv("ALLOWED_DEVICE_IDS", "")
    monkeypatch.setenv("RELAY_DEVICE_TOKENS", "")
    monkeypatch.setenv("PIGEON_RATE_LIMIT_PER_MINUTE", "20")
    reload_settings()
    with TestClient(app) as c:
        yield c
    reload_settings()


def test_no_pin_list_uses_registered_devices_for_addressing(registry_client):
    headers_a = {"Authorization": "Bearer test-device-token", "X-Device-Id": "dev-a"}
    headers_b = {"Authorization": "Bearer test-device-token", "X-Device-Id": "dev-b"}

    # Before registration the recipient is not addressable.
    unknown = registry_client.post(
        "/api/v1/pigeon/send",
        headers=headers_a,
        json={"to": "dev-b", "text": "hi", "clientEventId": "pre-1"},
    )
    assert unknown.status_code == 404

    registry_client.post("/api/v1/pigeon/heartbeat", headers=headers_b, json={"name": "客厅"})
    registry_client.app.state.pigeon_presence._last_seen["dev-b"] = -1000.0
    listed = registry_client.post("/api/v1/pigeon/heartbeat", headers=headers_a, json={"name": "书房"}).json()
    assert [d["id"] for d in listed["devices"]] == ["dev-b"]
    assert listed["devices"][0]["online"] is False

    sent = registry_client.post(
        "/api/v1/pigeon/send",
        headers=headers_a,
        json={"to": "dev-b", "text": "注册后离线可发", "clientEventId": "offline-2"},
    )
    assert sent.status_code == 200
    inbox = registry_client.get("/api/v1/pigeon/inbox", headers=headers_b).json()
    assert [m["text"] for m in inbox["messages"]] == ["注册后离线可发"]

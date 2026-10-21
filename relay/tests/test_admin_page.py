"""Setup page (/admin): loopback-only access and the credential payload."""

from __future__ import annotations

from fastapi.testclient import TestClient

from relay import admin_page
from relay.main import app


def test_admin_endpoints_reject_non_loopback_without_token():
    client = TestClient(app)  # TestClient's peer address is "testclient", not loopback
    assert client.get("/admin").status_code == 403
    assert client.get("/admin/status").status_code == 403


def test_admin_status_accepts_admin_token():
    client = TestClient(app)
    response = client.get("/admin/status", headers={"X-Admin-Token": "test-admin-token"})
    assert response.status_code == 200
    body = response.json()
    assert body["deviceToken"] == "test-device-token"
    assert body["deviceId"] == "debug-client"
    assert body["relayUrl"].endswith("/api/v1/voice/stream")
    assert "asr" in body["status"]


def test_admin_page_renders_for_loopback(monkeypatch):
    monkeypatch.setattr(admin_page, "_is_loopback", lambda request: True)
    client = TestClient(app)
    page = client.get("/admin")
    assert page.status_code == 200
    assert "Voice Relay" in page.text
    status = client.get("/admin/status")
    assert status.status_code == 200
    assert status.json()["deviceToken"] == "test-device-token"

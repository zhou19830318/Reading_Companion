"""Server-managed device registry and config pull."""

from __future__ import annotations

from pathlib import Path

from fastapi.testclient import TestClient

from relay.config import reload_settings
from relay.main import app

ADMIN = {"X-Admin-Token": "test-admin-token"}


def _headers(token: str, device_id: str) -> dict[str, str]:
    return {"Authorization": f"Bearer {token}", "X-Device-Id": device_id}


def test_admin_manages_device_and_config_pull(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("RELAY_DEVICE_DIR", str(tmp_path / "devices"))
    monkeypatch.setenv("RELAY_DEVICE_TOKEN", "")
    monkeypatch.setenv("RELAY_DEVICE_TOKENS", "")
    monkeypatch.setenv("ALLOWED_DEVICE_IDS", "")
    reload_settings()

    try:
        with TestClient(app) as client:
            created = client.post(
                "/admin/devices",
                headers={**ADMIN, "Content-Type": "application/json"},
                json={
                    "deviceId": "onepage-001",
                    "name": "客厅",
                    "relayUrl": "wss://relay.example/api/v1/voice/stream",
                },
            )
            assert created.status_code == 200
            device = created.json()["device"]
            token = device["token"]
            assert token

            config = client.get("/api/v1/device/config", headers=_headers(token, "onepage-001"))
            assert config.status_code == 200
            body = config.json()
            assert body["relay"] == {
                "url": "wss://relay.example/api/v1/voice/stream",
                "deviceId": "onepage-001",
                "token": token,
            }

            listed = client.get("/admin/devices", headers=ADMIN).json()["devices"]
            assert [row["deviceId"] for row in listed] == ["onepage-001"]

            assert client.get("/api/v1/device/config", headers=_headers("wrong-token-123456", "onepage-001")).status_code == 401
            assert client.get("/api/v1/device/config", headers=_headers(token, "unknown-device")).status_code == 401
            assert client.post(
                "/admin/devices",
                headers={**ADMIN, "Content-Type": "application/json"},
                json={"deviceId": "x" * 65, "deviceToken": "long-id-token-123456"},
            ).status_code == 400
            assert client.post(
                "/admin/devices",
                headers={**ADMIN, "Content-Type": "application/json"},
                json={"deviceId": "onepage-009", "deviceToken": "t" * 193},
            ).status_code == 400
            assert client.post(
                "/admin/devices",
                headers={**ADMIN, "Content-Type": "application/json"},
                json={"deviceId": "onepage-009", "relayUrl": "http://relay.example"},
            ).status_code == 400

            deleted = client.delete("/admin/devices?deviceId=onepage-001", headers=ADMIN)
            assert deleted.status_code == 200
            assert deleted.json()["deleted"] is True
            assert client.get("/api/v1/device/config", headers=_headers(token, "onepage-001")).status_code == 401
    finally:
        reload_settings()


def test_token_rotation_keeps_previous_token_for_config_pull(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("RELAY_DEVICE_DIR", str(tmp_path / "devices"))
    monkeypatch.setenv("RELAY_DEVICE_TOKEN", "")
    monkeypatch.setenv("RELAY_DEVICE_TOKENS", "")
    monkeypatch.setenv("ALLOWED_DEVICE_IDS", "")
    reload_settings()

    try:
        with TestClient(app) as client:
            created = client.post(
                "/admin/devices",
                headers={**ADMIN, "Content-Type": "application/json"},
                json={"deviceId": "onepage-002", "deviceToken": "old-token-1234567890"},
            )
            assert created.status_code == 200

            rotated = client.post(
                "/admin/devices",
                headers={**ADMIN, "Content-Type": "application/json"},
                json={"deviceId": "onepage-002", "deviceToken": "new-token-1234567890"},
            )
            assert rotated.status_code == 200

            pulled = client.get("/api/v1/device/config", headers=_headers("old-token-1234567890", "onepage-002"))
            assert pulled.status_code == 200
            assert pulled.json()["relay"]["token"] == "new-token-1234567890"

            assert client.get("/api/v1/device/config", headers=_headers("new-token-1234567890", "onepage-002")).status_code == 200
    finally:
        reload_settings()

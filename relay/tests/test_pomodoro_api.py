"""Pomodoro API: device auth, idempotent session upload, daily stats."""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from relay.config import reload_settings
from relay.main import app

DEVICE = {"Authorization": "Bearer test-device-token", "X-Device-Id": "debug-client"}


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("RELAY_POMODORO_DIR", str(tmp_path / "pomodoro"))
    reload_settings()
    with TestClient(app) as c:
        yield c
    reload_settings()


def _session(cid: str, kind: str = "work", day: str = "2026-10-29", minutes: int = 25, completed: bool = True):
    return {
        "clientEventId": cid,
        "kind": kind,
        "startedAt": f"{day}T09:00:00+08:00",
        "endedAt": f"{day}T09:25:00+08:00",
        "minutes": minutes,
        "completed": completed,
    }


def test_requires_device_token(client):
    assert client.post("/api/v1/pomodoro/sessions", json={"sessions": [_session("e1")]}).status_code == 401
    assert client.get("/api/v1/pomodoro/stats").status_code == 401


def test_session_upload_and_idempotent_retry(client):
    payload = {"sessions": [_session("s1"), _session("s2", "break", minutes=5)]}
    resp = client.post("/api/v1/pomodoro/sessions", headers=DEVICE, json=payload)
    assert resp.status_code == 200
    assert resp.json() == {"ok": True, "accepted": 2, "stored": 2}

    # Retry with the same clientEventIds (lost ack): stored stays 0.
    again = client.post("/api/v1/pomodoro/sessions", headers=DEVICE, json=payload)
    assert again.json() == {"ok": True, "accepted": 2, "stored": 0}

    # Mixed batch: only the new id lands.
    mixed = client.post(
        "/api/v1/pomodoro/sessions",
        headers=DEVICE,
        json={"sessions": [_session("s1"), _session("s3", "long_break", minutes=15)]},
    )
    assert mixed.json() == {"ok": True, "accepted": 2, "stored": 1}


def test_session_validation(client):
    empty_time = _session("e1")
    empty_time["startedAt"] = ""
    bad = [
        {"sessions": []},
        {"sessions": [_session("e%d" % i) for i in range(51)]},  # over batch max
        {"sessions": [_session("", kind="work")]},  # missing clientEventId
        {"sessions": [_session("e1", kind="meditate")]},  # unknown kind
        {"sessions": [empty_time]},  # empty startedAt
        {"sessions": [_session("e1", minutes=0)]},  # below MIN_MINUTES
        {"sessions": [_session("e1", minutes=181)]},  # above MAX_MINUTES
    ]
    for body in bad:
        resp = client.post("/api/v1/pomodoro/sessions", headers=DEVICE, json=body)
        assert resp.status_code == 400, body


def test_stats_aggregates_work_minutes(client):
    client.post(
        "/api/v1/pomodoro/sessions",
        headers=DEVICE,
        json={
            "sessions": [
                _session("w1", "work", "2026-10-28", 25),
                _session("w2", "work", "2026-10-28", 45, completed=False),
                _session("b1", "break", "2026-10-28", 5),
                _session("w3", "work", "2026-10-29", 15),
            ]
        },
    )
    body = client.get("/api/v1/pomodoro/stats", headers=DEVICE, params={"days": 366}).json()
    by_date = {d["date"]: d for d in body["days"]}
    assert by_date["2026-10-28"] == {
        "date": "2026-10-28",
        "focusMinutes": 70,
        "sessions": 2,
        "completed": 1,  # w2 ended early
    }
    assert by_date["2026-10-29"]["focusMinutes"] == 15
    assert body["totals"] == {"focusMinutes": 85, "sessions": 3, "completed": 2}


def test_stats_days_clamped(client):
    assert client.get("/api/v1/pomodoro/stats", headers=DEVICE, params={"days": 0}).json()["days"] == []
    resp = client.get("/api/v1/pomodoro/stats", headers=DEVICE, params={"days": 99999})
    assert resp.status_code == 200  # clamped to 366, not rejected


def test_device_config_exposes_pomodoro_feature(client):
    body = client.get("/api/v1/device/config", headers=DEVICE).json()
    # Firmware hides its focus-timer menu when the key is absent (older
    # relay) or false.
    assert body["features"]["pomodoro"] is True

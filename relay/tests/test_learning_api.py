"""Learning API: device auth, word book sync, idempotent review events."""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from relay.config import reload_settings
from relay.main import app

DEVICE = {"Authorization": "Bearer test-device-token", "X-Device-Id": "debug-client"}


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("RELAY_LEARNING_DIR", str(tmp_path / "learning"))
    reload_settings()
    with TestClient(app) as c:
        yield c
    reload_settings()


def test_requires_device_token(client):
    assert client.get("/api/v1/learning/words").status_code == 401
    assert (
        client.post("/api/v1/learning/words", json={"words": ["hello"]}).status_code == 401
    )
    assert (
        client.post(
            "/api/v1/learning/review",
            json={"word": "hello", "rating": 3, "clientEventId": "e1"},
        ).status_code
        == 401
    )


def test_word_batch_add_and_list(client):
    resp = client.post(
        "/api/v1/learning/words",
        headers=DEVICE,
        json={"words": ["abandon", "ability", "abandon", "  "], "source": "lookup"},
    )
    assert resp.status_code == 200
    # accepted echoes input length; added counts rows (dupe + blank filtered).
    assert resp.json() == {"ok": True, "accepted": 4, "added": 2}

    again = client.post("/api/v1/learning/words", headers=DEVICE, json={"words": ["abandon"]})
    assert again.json()["added"] == 0  # idempotent retry

    listing = client.get("/api/v1/learning/words", headers=DEVICE).json()
    assert listing["count"] == 2
    words = {w["word"] for w in listing["words"]}
    assert words == {"abandon", "ability"}
    assert all(w["source"] == "lookup" for w in listing["words"])

    filtered = client.get("/api/v1/learning/words", headers=DEVICE, params={"state": "new"}).json()
    assert filtered["count"] == 0  # default state is 'active'


def test_word_batch_validation(client):
    assert client.post("/api/v1/learning/words", headers=DEVICE, json={"words": []}).status_code == 400
    too_many = ["w%d" % i for i in range(201)]
    assert client.post("/api/v1/learning/words", headers=DEVICE, json={"words": too_many}).status_code == 400


def test_review_idempotent_and_auto_adds_word(client):
    payload = {"word": "candid", "rating": 3, "clientEventId": "evt-1", "occurredAt": "2026-10-24T10:00:00Z"}
    first = client.post("/api/v1/learning/review", headers=DEVICE, json=payload)
    assert first.status_code == 200
    assert first.json() == {"ok": True, "duplicate": False}

    retry = client.post("/api/v1/learning/review", headers=DEVICE, json=payload)
    assert retry.json() == {"ok": True, "duplicate": True}

    other = client.post(
        "/api/v1/learning/review",
        headers=DEVICE,
        json={"word": "candid", "rating": 4, "clientEventId": "evt-2"},
    )
    assert other.json() == {"ok": True, "duplicate": False}

    # Review auto-created the word row (device forgot to sync it).
    listing = client.get("/api/v1/learning/words", headers=DEVICE).json()
    assert listing["count"] == 1
    assert listing["words"][0]["word"] == "candid"
    assert listing["words"][0]["source"] == "review"


def test_review_queue_returns_due_words_and_total(client):
    client.post("/api/v1/learning/words", headers=DEVICE, json={"words": ["abandon", "ability", "candid"]})
    # Reviewed with 认识 (3) = not due again for 3 days.
    client.post(
        "/api/v1/learning/review",
        headers=DEVICE,
        json={"word": "candid", "rating": 3, "clientEventId": "evt-candid"},
    )

    body = client.get("/api/v1/learning/review", headers=DEVICE).json()
    assert body["due"] == 2
    assert body["count"] == 2
    assert {w["word"] for w in body["words"]} == {"abandon", "ability"}
    assert all(w["lastRating"] is None for w in body["words"])

    limited = client.get("/api/v1/learning/review", headers=DEVICE, params={"limit": 1}).json()
    assert limited["count"] == 1
    assert limited["due"] == 2


def test_review_queue_requires_token(client):
    assert client.get("/api/v1/learning/review").status_code == 401


def test_review_validation(client):
    bad = [
        {"word": "", "rating": 3, "clientEventId": "e"},
        {"word": "x", "rating": 0, "clientEventId": "e"},
        {"word": "x", "rating": 5, "clientEventId": "e"},
        {"word": "x", "rating": 3, "clientEventId": ""},
    ]
    for body in bad:
        resp = client.post("/api/v1/learning/review", headers=DEVICE, json=body)
        assert resp.status_code == 400, body


def test_list_limit_clamped(client):
    words = ["w%03d" % i for i in range(10)]
    client.post("/api/v1/learning/words", headers=DEVICE, json={"words": words})
    resp = client.get("/api/v1/learning/words", headers=DEVICE, params={"limit": 9999})
    assert resp.status_code == 200
    assert resp.json()["count"] == 10  # clamp only caps at 500, 10 stays

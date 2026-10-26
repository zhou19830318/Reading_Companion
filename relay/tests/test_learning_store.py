"""LearningStore: schema idempotency, path resolution, lifespan wiring."""

from __future__ import annotations

from pathlib import Path

from fastapi.testclient import TestClient

from relay.config import get_settings, reload_settings
from relay.learning_store import SCHEMA_VERSION, LearningStore
from relay.main import app

DEVICE = {"Authorization": "Bearer test-device-token", "X-Device-Id": "debug-client"}


def test_open_creates_schema_and_is_idempotent(tmp_path):
    db = tmp_path / "learning.db"
    store = LearningStore(db)
    store.open()
    try:
        assert db.exists()
        assert store.schema_version() == SCHEMA_VERSION
        tables = {
            row["name"]
            for row in store.conn.execute("SELECT name FROM sqlite_master WHERE type='table'")
        }
        assert {"meta", "words", "review_events"} <= tables
    finally:
        store.close()

    # Re-open the same file: schema creation must not fail or duplicate.
    again = LearningStore(db)
    again.open()
    try:
        assert again.schema_version() == SCHEMA_VERSION
    finally:
        again.close()


def test_unique_client_event_id_enforced(tmp_path):
    store = LearningStore(tmp_path / "learning.db")
    store.open()
    try:
        conn = store.conn
        conn.execute(
            "INSERT INTO review_events(device_id, word, rating, client_event_id, created_at) "
            "VALUES('dev', 'hello', 3, 'evt-1', '2026-10-24T00:00:00+00:00')"
        )
        conn.commit()
        try:
            conn.execute(
                "INSERT INTO review_events(device_id, word, rating, client_event_id, created_at) "
                "VALUES('dev', 'hello', 4, 'evt-1', '2026-10-24T00:00:01+00:00')"
            )
            conn.commit()
        except Exception as exc:  # sqlite3.IntegrityError (import kept local to the test)
            assert "UNIQUE" in str(exc)
        else:
            raise AssertionError("duplicate client_event_id must be rejected")
        count = conn.execute("SELECT COUNT(*) AS c FROM review_events").fetchone()["c"]
        assert count == 1
    finally:
        store.close()


def test_review_queue_intervals_and_ordering(tmp_path):
    from datetime import datetime, timedelta, timezone

    store = LearningStore(tmp_path / "learning.db")
    store.open()
    try:
        store.add_words("dev", [("new", "lookup"), ("missed", "lookup"), ("known", "lookup")])
        now = datetime(2026, 10, 25, 12, 0, tzinfo=timezone.utc)
        store.conn.execute(
            "INSERT INTO review_events(device_id, word, rating, client_event_id, created_at) "
            "VALUES('dev', 'missed', 1, 'e1', ?)",
            ((now - timedelta(hours=2)).isoformat(timespec="seconds"),),
        )
        store.conn.execute(
            "INSERT INTO review_events(device_id, word, rating, client_event_id, created_at) "
            "VALUES('dev', 'known', 3, 'e2', ?)",
            ((now - timedelta(hours=2)).isoformat(timespec="seconds"),),
        )
        store.conn.commit()

        due, total = store.review_queue("dev", now=now)
        assert total == 2
        assert [row["word"] for row in due] == ["missed", "new"]
        assert due[0]["lastRating"] == 1
        assert due[1]["lastRating"] is None

        later = now + timedelta(days=3)
        due, total = store.review_queue("dev", now=later)
        assert total == 3
        assert {row["word"] for row in due} == {"new", "missed", "known"}
    finally:
        store.close()


def test_remove_words_tombstone_and_reactivate(tmp_path):
    store = LearningStore(tmp_path / "learning.db")
    store.open()
    try:
        assert store.add_words("dev", [("rust", "lookup")]) == 1
        assert store.remove_words("dev", ["rust", "ghost"]) == 1
        assert store.list_words("dev", state="active") == []
        assert [row["word"] for row in store.list_words("dev", state="removed")] == ["rust"]
        # Second removal is a no-op (already a tombstone).
        assert store.remove_words("dev", ["rust"]) == 0
        # Re-add reactivates; the return value still counts only new rows.
        assert store.add_words("dev", [("rust", "lookup")]) == 0
        assert [row["word"] for row in store.list_words("dev", state="active")] == ["rust"]
        # Another device's removal never touches this device's book.
        assert store.remove_words("other", ["rust"]) == 0
    finally:
        store.close()


def test_config_learning_db_path(monkeypatch, tmp_path):
    monkeypatch.setenv("RELAY_LEARNING_DIR", str(tmp_path / "learning"))
    reload_settings()
    try:
        settings = get_settings()
        assert settings.learning_db_path == tmp_path / "learning" / "learning.db"
    finally:
        reload_settings()

    # Empty (unset) = run/learning relative to the working directory.
    monkeypatch.delenv("RELAY_LEARNING_DIR", raising=False)
    reload_settings()
    try:
        settings = get_settings()
        assert settings.learning_db_path == Path("run/learning") / "learning.db"
    finally:
        reload_settings()


def test_lifespan_opens_store(tmp_path, monkeypatch):
    monkeypatch.setenv("RELAY_LEARNING_DIR", str(tmp_path / "learning"))
    reload_settings()
    try:
        with TestClient(app) as client:
            store: LearningStore = app.state.learning
            assert store.db_path == tmp_path / "learning" / "learning.db"
            assert store.schema_version() == SCHEMA_VERSION
            # Auth contract already exercised elsewhere; health stays open.
            assert client.get("/healthz").status_code == 200
    finally:
        reload_settings()

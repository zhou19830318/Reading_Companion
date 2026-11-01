"""PomodoroStore: schema idempotency, idempotent session upload, stats."""

from __future__ import annotations

from pathlib import Path

from relay.pomodoro_store import SCHEMA_VERSION, PomodoroStore


def _entry(cid: str, kind: str = "work", started: str = "2026-10-29T09:00:00", minutes: int = 25) -> dict:
    return {
        "client_event_id": cid,
        "kind": kind,
        "started_at": started,
        "ended_at": started.replace("09:00", "09:25"),
        "minutes": minutes,
        "completed": 1,
    }


def test_open_creates_schema_and_is_idempotent(tmp_path):
    db = tmp_path / "pomodoro.db"
    store = PomodoroStore(db)
    store.open()
    try:
        assert db.exists()
        assert store.schema_version() == SCHEMA_VERSION
        tables = {
            row["name"]
            for row in store.conn.execute("SELECT name FROM sqlite_master WHERE type='table'")
        }
        assert {"meta", "sessions"} <= tables
    finally:
        store.close()

    again = PomodoroStore(db)
    again.open()
    try:
        assert again.schema_version() == SCHEMA_VERSION
    finally:
        again.close()


def test_unique_client_event_id_enforced(tmp_path):
    store = PomodoroStore(tmp_path / "pomodoro.db")
    store.open()
    try:
        conn = store.conn
        stored = store.record_sessions("dev", [_entry("evt-1")])
        assert stored == 1
        try:
            conn.execute(
                "INSERT INTO sessions(device_id, client_event_id, kind, started_at, ended_at, minutes, created_at)"
                " VALUES('dev', 'evt-1', 'work', '2026-10-29T10:00:00', '2026-10-29T10:25:00', 25, '2026-10-29T10:25:00')"
            )
            conn.commit()
        except Exception as exc:  # sqlite3.IntegrityError (import kept local to the test)
            assert "UNIQUE" in str(exc)
        else:
            raise AssertionError("duplicate client_event_id must be rejected")
        count = conn.execute("SELECT COUNT(*) AS c FROM sessions").fetchone()["c"]
        assert count == 1
    finally:
        store.close()


def test_record_sessions_idempotent_retry(tmp_path):
    store = PomodoroStore(tmp_path / "pomodoro.db")
    store.open()
    try:
        entries = [_entry("a"), _entry("b")]
        assert store.record_sessions("dev", entries) == 2
        # Retry (device re-sends after a lost ack): nothing new stored.
        assert store.record_sessions("dev", entries) == 0
        # Intra-batch duplicate: second copy is ignored.
        assert store.record_sessions("dev", [_entry("a"), _entry("c")]) == 1
        assert store.record_sessions("dev", []) == 0
    finally:
        store.close()


def test_stats_groups_work_sessions_by_day(tmp_path):
    store = PomodoroStore(tmp_path / "pomodoro.db")
    store.open()
    try:
        store.record_sessions(
            "dev",
            [
                _entry("w1", "work", "2026-10-28T09:00:00", 25),
                _entry("w2", "work", "2026-10-28T14:00:00", 45),
                _entry("b1", "break", "2026-10-28T09:25:00", 5),
                _entry("w3", "work", "2026-10-29T09:00:00", 15),
            ],
        )
        stats = store.stats("dev", days=366)
        by_date = {d["date"]: d for d in stats["days"]}
        assert by_date["2026-10-28"] == {
            "date": "2026-10-28",
            "focusMinutes": 70,  # break minutes excluded
            "sessions": 2,
            "completed": 2,
        }
        assert by_date["2026-10-29"]["focusMinutes"] == 15
        assert stats["totals"] == {"focusMinutes": 85, "sessions": 3, "completed": 3}
        # Device scoping: another device sees nothing.
        assert store.stats("other", days=366)["days"] == []
    finally:
        store.close()


def test_stats_window_excludes_old_days(tmp_path):
    store = PomodoroStore(tmp_path / "pomodoro.db")
    store.open()
    try:
        store.record_sessions(
            "dev",
            [
                _entry("old", "work", "2000-01-01T09:00:00", 25),
                _entry("new", "work", "2099-01-01T09:00:00", 25),
            ],
        )
        stats = store.stats("dev", days=30)
        # Only the recent day survives the 30-day window.
        assert [d["date"] for d in stats["days"]] == ["2099-01-01"]
    finally:
        store.close()


def test_list_sessions_returns_newest_first(tmp_path):
    store = PomodoroStore(Path(tmp_path) / "pomodoro.db")
    store.open()
    try:
        store.record_sessions("dev", [_entry("first"), _entry("second")])
        rows = store.list_sessions("dev")
        assert [r["client_event_id"] for r in rows] == ["second", "first"]
        assert store.list_sessions("dev", limit=1)[0]["client_event_id"] == "second"
        assert store.list_sessions("other") == []
    finally:
        store.close()

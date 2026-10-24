"""SQLite-backed store for the vocabulary learning feature.

Design notes:
- One connection per process, opened in the FastAPI lifespan (single event
  loop, ms-scale statements — same sync-IO-in-handler precedent as
  dict_source/t2i). WAL keeps readers from blocking the writer if a second
  consumer (admin export) appears later.
- Idempotent schema creation (CREATE TABLE IF NOT EXISTS + INSERT OR IGNORE
  version), so a fresh checkout, a test, and a production upgrade all converge
  on the same code path.
- Review sync de-duplication is a UNIQUE constraint on
  review_events.client_event_id: the client retries with the same id and
  INSERT OR IGNORE decides — no application-level bookkeeping to drift.
"""

from __future__ import annotations

import sqlite3
from pathlib import Path

SCHEMA_VERSION = 1

_SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS words (
    device_id TEXT NOT NULL,
    word      TEXT NOT NULL,
    added_at  TEXT NOT NULL,
    state     TEXT NOT NULL DEFAULT 'active',
    source    TEXT NOT NULL DEFAULT 'lookup',
    extras    TEXT NOT NULL DEFAULT '{}',
    PRIMARY KEY (device_id, word)
);
CREATE TABLE IF NOT EXISTS review_events (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id       TEXT NOT NULL,
    word            TEXT NOT NULL,
    rating          INTEGER NOT NULL,
    client_event_id TEXT NOT NULL UNIQUE,
    created_at      TEXT NOT NULL,
    extras          TEXT NOT NULL DEFAULT '{}'
);
CREATE INDEX IF NOT EXISTS idx_review_events_device_time
    ON review_events (device_id, created_at);
"""


class LearningStore:
    def __init__(self, db_path: Path) -> None:
        self.db_path = Path(db_path)
        self._conn: sqlite3.Connection | None = None

    def open(self) -> None:
        self.db_path.parent.mkdir(parents=True, exist_ok=True)
        # check_same_thread=False: async handlers and lifespan share the event
        # loop thread in production, but TestClient runs them on different
        # threads (as would any future sync export/admin job). All statements
        # are ms-scale and the process is the only writer, so cross-thread use
        # of the one connection is safe (SQLite's default serialized threading
        # mode applies).
        conn = sqlite3.connect(self.db_path, timeout=5.0, check_same_thread=False)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA busy_timeout=5000")
        conn.execute("PRAGMA synchronous=NORMAL")
        conn.executescript(_SCHEMA)
        # First writer wins the version; later code compares and migrates
        # (never silently downgrades an existing database).
        conn.execute("INSERT OR IGNORE INTO meta(key, value) VALUES('schema_version', ?)", (str(SCHEMA_VERSION),))
        conn.commit()
        self._conn = conn

    @property
    def conn(self) -> sqlite3.Connection:
        if self._conn is None:
            raise RuntimeError("LearningStore is not open (lifespan did not run?)")
        return self._conn

    def schema_version(self) -> int:
        row = self.conn.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
        return int(row["value"]) if row else 0

    def close(self) -> None:
        if self._conn is not None:
            self._conn.close()
            self._conn = None

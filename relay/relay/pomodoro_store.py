"""SQLite-backed store for the pomodoro focus-timer feature.

Design notes mirror learning_store.py:
- One connection per process, opened in the FastAPI lifespan (single event
  loop, ms-scale statements — same sync-IO-in-handler precedent as
  dict_source/t2i/learning). WAL keeps readers from blocking the writer.
- Idempotent schema creation (CREATE TABLE IF NOT EXISTS + INSERT OR IGNORE
  version), so a fresh checkout, a test, and a production upgrade all converge
  on the same code path.
- Session sync de-duplication is a UNIQUE constraint on
  sessions.client_event_id: the client retries with the same id and
  INSERT OR IGNORE decides — no application-level bookkeeping to drift.
"""

from __future__ import annotations

import sqlite3
from datetime import datetime, timedelta, timezone
from pathlib import Path

SCHEMA_VERSION = 1

# Session kinds the device may report. Only 'work' counts toward focus
# stats; breaks are stored for history (device-side cycle reconstruction,
# future heatmap) but excluded from focusMinutes.
SESSION_KINDS = ("work", "break", "long_break")

# Bounds enforced both here and at the API edge (device timers are user
# presets of 1-180 min; 0/negative means a clock bug, not a real session).
MIN_MINUTES = 1
MAX_MINUTES = 180


def _now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


_SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS sessions (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id       TEXT NOT NULL,
    client_event_id TEXT NOT NULL UNIQUE,
    kind            TEXT NOT NULL,
    started_at      TEXT NOT NULL,
    ended_at        TEXT NOT NULL,
    minutes         INTEGER NOT NULL,
    completed       INTEGER NOT NULL DEFAULT 1,
    created_at      TEXT NOT NULL,
    extras          TEXT NOT NULL DEFAULT '{}'
);
CREATE INDEX IF NOT EXISTS idx_sessions_device_time
    ON sessions (device_id, started_at);
"""


class PomodoroStore:
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
            raise RuntimeError("PomodoroStore is not open (lifespan did not run?)")
        return self._conn

    def schema_version(self) -> int:
        row = self.conn.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
        return int(row["value"]) if row else 0

    def record_sessions(self, device_id: str, entries: list[dict]) -> int:
        """Insert completed session records; returns rows newly stored.

        Each entry: client_event_id, kind, started_at, ended_at, minutes,
        completed (0/1). INSERT OR IGNORE makes retries and intra-batch
        duplicates no-ops; `stored` counts only newly inserted rows so the
        API can echo the difference between accepted and stored.
        """
        if not entries:
            return 0
        conn = self.conn
        cur = conn.executemany(
            "INSERT OR IGNORE INTO sessions"
            "(device_id, client_event_id, kind, started_at, ended_at, minutes, completed, created_at)"
            " VALUES(?,?,?,?,?,?,?,?)",
            [
                (
                    device_id,
                    e["client_event_id"],
                    e["kind"],
                    e["started_at"],
                    e["ended_at"],
                    e["minutes"],
                    e["completed"],
                    _now(),
                )
                for e in entries
            ],
        )
        stored = cur.rowcount
        conn.commit()
        return stored

    def list_sessions(self, device_id: str, limit: int = 50) -> list[dict]:
        rows = self.conn.execute(
            "SELECT client_event_id, kind, started_at, ended_at, minutes, completed, created_at"
            " FROM sessions WHERE device_id = ? ORDER BY id DESC LIMIT ?",
            (device_id, limit),
        ).fetchall()
        return [dict(row) for row in rows]

    def stats(self, device_id: str, days: int = 30) -> dict:
        """Per-day focus aggregates for the last `days` calendar days.

        Grouping uses the date prefix of started_at exactly as the device
        wrote it (its local day); the window cutoff is derived from the
        server's UTC date. The two can differ by hours at the edges —
        acceptable for a reading-friendliness stat, and it keeps the server
        out of the device's timezone business entirely.
        Only days with at least one 'work' session appear (sparse — the
        client zero-fills for heatmaps).
        """
        since = (datetime.now(timezone.utc) - timedelta(days=max(1, days) - 1)).strftime("%Y-%m-%d")
        rows = self.conn.execute(
            """
            SELECT substr(started_at, 1, 10) AS date,
                   SUM(CASE WHEN kind = 'work' THEN minutes ELSE 0 END) AS focus_minutes,
                   SUM(CASE WHEN kind = 'work' THEN 1 ELSE 0 END) AS sessions,
                   SUM(CASE WHEN kind = 'work' AND completed = 1 THEN 1 ELSE 0 END) AS completed
              FROM sessions
             WHERE device_id = ? AND substr(started_at, 1, 10) >= ?
             GROUP BY date
             ORDER BY date
            """,
            (device_id, since),
        ).fetchall()
        days_out = [
            {
                "date": row["date"],
                "focusMinutes": int(row["focus_minutes"] or 0),
                "sessions": int(row["sessions"] or 0),
                "completed": int(row["completed"] or 0),
            }
            for row in rows
        ]
        return {
            "days": days_out,
            "totals": {
                "focusMinutes": sum(d["focusMinutes"] for d in days_out),
                "sessions": sum(d["sessions"] for d in days_out),
                "completed": sum(d["completed"] for d in days_out),
            },
        }

    def close(self) -> None:
        if self._conn is not None:
            self._conn.close()
            self._conn = None

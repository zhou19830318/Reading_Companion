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

import json
import sqlite3
from datetime import datetime, timedelta, timezone
from pathlib import Path

SCHEMA_VERSION = 1

# Review spacing by rating (百词斩 §6.4 uses 忘记/模糊/认识 = 1/2/3; 4 is the
# web's "easy"). 忘记 comes back inside the same session day so a missed word
# is seen again before the day ends.
REVIEW_INTERVALS = {
    1: timedelta(hours=1),
    2: timedelta(days=1),
    3: timedelta(days=3),
    4: timedelta(days=7),
}


def _now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _dumps(extras: dict | None) -> str:
    return json.dumps(extras or {}, ensure_ascii=False, separators=(",", ":"))

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

    def add_words(self, device_id: str, entries: list[tuple[str, str]]) -> int:
        """Insert word-book entries as (word, source); returns rows added.

        INSERT OR IGNORE makes retries and intra-batch duplicates no-ops.
        """
        cleaned = [(word.strip(), (source or "lookup").strip() or "lookup") for word, source in entries]
        cleaned = [(word, source) for word, source in cleaned if word]
        if not cleaned:
            return 0
        conn = self.conn
        cur = conn.executemany(
            "INSERT OR IGNORE INTO words(device_id, word, added_at, source) VALUES(?,?,?,?)",
            [(device_id, word, _now(), source) for word, source in cleaned],
        )
        conn.commit()
        return cur.rowcount

    def list_words(self, device_id: str, state: str | None = None, limit: int = 500) -> list[dict]:
        sql = "SELECT word, added_at, state, source FROM words WHERE device_id = ?"
        params: list = [device_id]
        if state:
            sql += " AND state = ?"
            params.append(state)
        sql += " ORDER BY added_at DESC, word LIMIT ?"
        params.append(limit)
        return [dict(row) for row in self.conn.execute(sql, params)]

    def record_review(
        self,
        device_id: str,
        word: str,
        rating: int,
        client_event_id: str,
        extras: dict | None = None,
    ) -> bool:
        """Store one review event; False when client_event_id already exists.

        The word row is auto-created so a review for a word the device
        forgot to sync still lands in the book view (source='review').
        Both statements commit together — no half-written review.
        """
        conn = self.conn
        cur = conn.execute(
            "INSERT OR IGNORE INTO review_events"
            "(device_id, word, rating, client_event_id, created_at, extras) VALUES(?,?,?,?,?,?)",
            (device_id, word, rating, client_event_id, _now(), _dumps(extras)),
        )
        conn.execute(
            "INSERT OR IGNORE INTO words(device_id, word, added_at, source) VALUES(?,?,?,?)",
            (device_id, word, _now(), "review"),
        )
        conn.commit()
        return cur.rowcount == 1

    def review_count(self, device_id: str) -> int:
        row = self.conn.execute("SELECT COUNT(*) AS c FROM review_events WHERE device_id = ?", (device_id,)).fetchone()
        return int(row["c"])

    def review_queue(
        self,
        device_id: str,
        now: datetime | None = None,
        limit: int = 20,
    ) -> tuple[list[dict], int]:
        """Words whose last review has expired, oldest due first.

        Returns (page, total_due). `now` is injectable for tests; production
        callers omit it. A word without any review is due immediately, so a
        fresh word book starts a queue as soon as it is looked up.
        """
        moment = now or datetime.now(timezone.utc)
        rows = self.conn.execute(
            """
            SELECT w.word, w.added_at, w.source,
                   (SELECT e.rating FROM review_events e
                     WHERE e.device_id = w.device_id AND e.word = w.word
                     ORDER BY e.id DESC LIMIT 1) AS rating,
                   (SELECT e.created_at FROM review_events e
                     WHERE e.device_id = w.device_id AND e.word = w.word
                     ORDER BY e.id DESC LIMIT 1) AS last_review
              FROM words w
             WHERE w.device_id = ? AND w.state = 'active'
            """,
            (device_id,),
        ).fetchall()

        due: list[dict] = []
        for row in rows:
            last_review = row["last_review"]
            if last_review:
                try:
                    last = datetime.fromisoformat(last_review)
                except ValueError:
                    last = None
                if last is not None:
                    if last.tzinfo is None:
                        last = last.replace(tzinfo=timezone.utc)
                    interval = REVIEW_INTERVALS.get(int(row["rating"] or 1), REVIEW_INTERVALS[1])
                    due_at = last + interval
                    if due_at > moment:
                        continue
                else:
                    due_at = moment
            else:
                due_at = moment
            due.append(
                {
                    "word": row["word"],
                    "addedAt": row["added_at"],
                    "source": row["source"],
                    "lastRating": int(row["rating"]) if row["rating"] is not None else None,
                    "lastReviewedAt": last_review,
                    "dueAt": due_at.isoformat(timespec="seconds"),
                }
            )

        due.sort(key=lambda item: (item["dueAt"], item["word"]))
        return due[:limit], len(due)

    def close(self) -> None:
        if self._conn is not None:
            self._conn.close()
            self._conn = None

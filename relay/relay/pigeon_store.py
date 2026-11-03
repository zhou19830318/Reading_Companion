"""Store-and-forward messages and in-memory presence for the pigeon feature.

Design notes mirror learning_store.py / pomodoro_store.py:
- One SQLite connection per process, opened in the FastAPI lifespan. The
  handlers are async but every statement is ms-scale and the process is the
  only writer, so the same sync-IO-in-handler precedent applies.
- Idempotent schema creation (CREATE TABLE IF NOT EXISTS + INSERT OR IGNORE
  version) keeps a fresh checkout, tests, and production upgrades on one path.
- Send de-duplication is a UNIQUE constraint on
  (sender_id, client_event_id): a retry with the same id returns the stored
  row instead of creating a second message.
- Presence is process-local and deliberately ephemeral. A relay restart loses
  the online list; devices re-register with their next heartbeat. Messages are
  durable, so a restart never loses a delivery.

Retention is enforced on send (and on open) rather than by a background job:
the relay has no scheduler and the per-recipient cap is tiny (100 rows).
"""

from __future__ import annotations

import sqlite3
import time
import uuid
from datetime import datetime, timedelta, timezone
from pathlib import Path

SCHEMA_VERSION = 2

DEFAULT_TTL_DAYS = 7
DEFAULT_MAX_PER_RECIPIENT = 100
DEFAULT_PRESENCE_TTL_SECONDS = 45.0


def _now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _cutoff(days: int) -> str:
    return (datetime.now(timezone.utc) - timedelta(days=max(1, days))).isoformat(timespec="seconds")


_SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS messages (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    sender_id       TEXT NOT NULL,
    recipient_id    TEXT NOT NULL,
    client_event_id TEXT NOT NULL,
    text            TEXT NOT NULL,
    created_at      TEXT NOT NULL,
    read_at         TEXT,
    UNIQUE(sender_id, client_event_id)
);
CREATE INDEX IF NOT EXISTS idx_messages_recipient_id
    ON messages (recipient_id, id);
CREATE INDEX IF NOT EXISTS idx_messages_created
    ON messages (created_at);
CREATE TABLE IF NOT EXISTS devices (
    device_id  TEXT PRIMARY KEY,
    name       TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    last_seen  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_devices_last_seen
    ON devices (last_seen);
"""


class PigeonStore:
    def __init__(
        self,
        db_path: Path,
        *,
        ttl_days: int = DEFAULT_TTL_DAYS,
        max_per_recipient: int = DEFAULT_MAX_PER_RECIPIENT,
    ) -> None:
        self.db_path = Path(db_path)
        self.ttl_days = max(1, int(ttl_days))
        self.max_per_recipient = max(1, int(max_per_recipient))
        self._conn: sqlite3.Connection | None = None

    def open(self) -> None:
        self.db_path.parent.mkdir(parents=True, exist_ok=True)
        conn = sqlite3.connect(self.db_path, timeout=5.0, check_same_thread=False)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA busy_timeout=5000")
        conn.execute("PRAGMA synchronous=NORMAL")
        conn.executescript(_SCHEMA)
        conn.execute("INSERT OR REPLACE INTO meta(key, value) VALUES('schema_version', ?)", (str(SCHEMA_VERSION),))
        conn.commit()
        self._conn = conn

    @property
    def conn(self) -> sqlite3.Connection:
        if self._conn is None:
            raise RuntimeError("PigeonStore is not open (lifespan did not run?)")
        return self._conn

    def schema_version(self) -> int:
        row = self.conn.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
        return int(row["value"]) if row else 0

    @staticmethod
    def _row(row: sqlite3.Row) -> dict:
        return {
            "id": int(row["id"]),
            "from": row["sender_id"],
            "to": row["recipient_id"],
            "text": row["text"],
            "createdAt": row["created_at"],
            "readAt": row["read_at"],
            "clientEventId": row["client_event_id"],
        }

    def touch_device(self, device_id: str, name: str = "") -> None:
        """Remember a device so it stays addressable while offline.

        The name is optional and last-writer-wins only when non-empty; a
        heartbeat without a name must not erase the one registered earlier.
        """
        device_id = (device_id or "").strip()
        if not device_id:
            return
        clean = (name or "").strip()[:32]
        now = _now()
        with self.conn:
            self.conn.execute(
                "INSERT OR IGNORE INTO devices(device_id, name, created_at, last_seen)"
                " VALUES(?,?,?,?)",
                (device_id, clean, now, now),
            )
            if clean:
                self.conn.execute(
                    "UPDATE devices SET name=?, last_seen=? WHERE device_id=?",
                    (clean, now, device_id),
                )
            else:
                self.conn.execute(
                    "UPDATE devices SET last_seen=? WHERE device_id=?",
                    (now, device_id),
                )

    def devices(self, exclude_id: str | None = None) -> list[dict]:
        rows = self.conn.execute(
            "SELECT device_id, name, last_seen FROM devices"
            " WHERE device_id<>? ORDER BY lower(name), device_id",
            ((exclude_id or ""),),
        ).fetchall()
        return [
            {"id": row["device_id"], "name": row["name"] or row["device_id"], "lastSeen": row["last_seen"]}
            for row in rows
        ]

    def has_device(self, device_id: str) -> bool:
        row = self.conn.execute("SELECT 1 FROM devices WHERE device_id=?", (device_id,)).fetchone()
        return row is not None

    def send(self, sender_id: str, recipient_id: str, text: str, client_event_id: str = "") -> dict:
        """Store one message and return its public row.

        An empty client_event_id is replaced with a random id; retries are
        idempotent only when the device supplies a stable value.
        """
        event_id = (client_event_id or "").strip() or uuid.uuid4().hex
        now = _now()
        with self.conn:
            if client_event_id:
                existing = self.conn.execute(
                    "SELECT * FROM messages WHERE sender_id=? AND client_event_id=?",
                    (sender_id, event_id),
                ).fetchone()
                if existing is not None:
                    out = self._row(existing)
                    out["duplicate"] = True
                    return out
            cur = self.conn.execute(
                "INSERT INTO messages(sender_id, recipient_id, client_event_id, text, created_at)"
                " VALUES(?,?,?,?,?)",
                (sender_id, recipient_id, event_id, text, now),
            )
            message_id = int(cur.lastrowid)
            self._prune(recipient_id)
        row = self.conn.execute("SELECT * FROM messages WHERE id=?", (message_id,)).fetchone()
        out = self._row(row)
        out["duplicate"] = False
        return out

    def _prune(self, recipient_id: str) -> None:
        # Age first (global), then the per-recipient cap. Both are bounded
        # DELETEs; the cap uses a subquery so it works on old SQLite builds
        # without window functions.
        self.conn.execute("DELETE FROM messages WHERE created_at < ?", (_cutoff(self.ttl_days),))
        self.conn.execute(
            "DELETE FROM messages WHERE recipient_id=? AND id NOT IN"
            " (SELECT id FROM messages WHERE recipient_id=? ORDER BY id DESC LIMIT ?)",
            (recipient_id, recipient_id, self.max_per_recipient),
        )

    def inbox(self, device_id: str, after_id: int = 0, limit: int = 50) -> list[dict]:
        rows = self.conn.execute(
            "SELECT * FROM messages WHERE recipient_id=? AND id>?"
            " ORDER BY id ASC LIMIT ?",
            (device_id, max(0, int(after_id)), max(1, min(int(limit), 100))),
        ).fetchall()
        return [self._row(row) for row in rows]

    def history(self, device_id: str, peer_id: str, limit: int = 50) -> list[dict]:
        rows = self.conn.execute(
            "SELECT * FROM messages"
            " WHERE (sender_id=? AND recipient_id=?) OR (sender_id=? AND recipient_id=?)"
            " ORDER BY id DESC LIMIT ?",
            (device_id, peer_id, peer_id, device_id, max(1, min(int(limit), 100))),
        ).fetchall()
        return [self._row(row) for row in reversed(rows)]

    def clear_conversation(self, device_id: str, peer_id: str) -> int:
        cur = self.conn.execute(
            "DELETE FROM messages"
            " WHERE (sender_id=? AND recipient_id=?) OR (sender_id=? AND recipient_id=?)",
            (device_id, peer_id, peer_id, device_id),
        )
        self.conn.commit()
        return int(cur.rowcount)

    def ack(self, device_id: str, ids: list[int] | None = None, up_to_id: int | None = None) -> int:
        ids = [int(i) for i in (ids or []) if int(i) > 0]
        if not ids and up_to_id is None:
            return 0
        where = "recipient_id=? AND read_at IS NULL"
        params: list[object] = [_now(), device_id]
        if up_to_id is not None:
            where += " AND id<=?"
            params.append(int(up_to_id))
        if ids:
            where += " AND id IN (" + ",".join("?" for _ in ids) + ")"
            params.extend(ids)
        cur = self.conn.execute(f"UPDATE messages SET read_at=? WHERE {where}", params)
        self.conn.commit()
        return int(cur.rowcount)

    def unread_count(self, device_id: str) -> int:
        row = self.conn.execute(
            "SELECT COUNT(*) AS n FROM messages WHERE recipient_id=? AND read_at IS NULL",
            (device_id,),
        ).fetchone()
        return int(row["n"])

    def close(self) -> None:
        if self._conn is not None:
            self._conn.close()
            self._conn = None


class PigeonPresence:
    """Process-local online registry, refreshed by device heartbeats."""

    def __init__(self, ttl_seconds: float = DEFAULT_PRESENCE_TTL_SECONDS) -> None:
        self.ttl_seconds = max(1.0, float(ttl_seconds))
        self._last_seen: dict[str, float] = {}
        self._names: dict[str, str] = {}

    def touch(self, device_id: str, name: str = "") -> None:
        if not device_id:
            return
        self._last_seen[device_id] = time.monotonic()
        clean = (name or "").strip()
        if clean:
            self._names[device_id] = clean[:32]

    def is_online(self, device_id: str) -> bool:
        seen = self._last_seen.get(device_id)
        return seen is not None and (time.monotonic() - seen) <= self.ttl_seconds

    def name(self, device_id: str) -> str:
        return self._names.get(device_id, "")

    def online_ids(self, exclude: str | None = None) -> list[str]:
        now = time.monotonic()
        return sorted(
            device_id
            for device_id, seen in self._last_seen.items()
            if device_id != exclude and (now - seen) <= self.ttl_seconds
        )

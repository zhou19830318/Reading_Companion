"""Pigeon store: delivery, idempotency, read acks, retention, presence."""

from __future__ import annotations

from relay.pigeon_store import PigeonPresence, PigeonStore


def test_send_inbox_ack_and_history(tmp_path):
    store = PigeonStore(tmp_path / "pigeon.db")
    store.open()
    try:
        first = store.send("dev-a", "dev-b", "你好", "e1")
        assert first["id"] > 0
        assert first["duplicate"] is False
        assert first["from"] == "dev-a"
        assert first["to"] == "dev-b"
        assert store.unread_count("dev-b") == 1

        assert store.inbox("dev-a") == []
        rows = store.inbox("dev-b")
        assert [r["text"] for r in rows] == ["你好"]

        assert store.ack("dev-b", ids=[first["id"]]) == 1
        assert store.unread_count("dev-b") == 0
        assert store.ack("dev-b", ids=[first["id"]]) == 0

        reply = store.send("dev-b", "dev-a", "收到", "e2")
        history = store.history("dev-a", "dev-b")
        assert [m["id"] for m in history] == [first["id"], reply["id"]]
    finally:
        store.close()


def test_clear_conversation_deletes_both_directions_only_for_that_peer(tmp_path):
    store = PigeonStore(tmp_path / "pigeon.db")
    store.open()
    try:
        store.send("dev-a", "dev-b", "hello", "e1")
        store.send("dev-b", "dev-a", "hi", "e2")
        store.send("dev-a", "dev-c", "other", "e3")

        assert store.clear_conversation("dev-a", "dev-b") == 2
        assert store.history("dev-a", "dev-b") == []
        assert store.history("dev-b", "dev-a") == []
        assert [m["text"] for m in store.history("dev-a", "dev-c")] == ["other"]
    finally:
        store.close()


def test_send_is_idempotent_per_sender_event_id(tmp_path):
    store = PigeonStore(tmp_path / "pigeon.db")
    store.open()
    try:
        first = store.send("dev-a", "dev-b", "hi", "same-event")
        again = store.send("dev-a", "dev-b", "hi", "same-event")
        assert again["id"] == first["id"]
        assert again["duplicate"] is True
        assert len(store.inbox("dev-b")) == 1
    finally:
        store.close()


def test_retention_caps_per_recipient(tmp_path):
    store = PigeonStore(tmp_path / "pigeon.db", max_per_recipient=2)
    store.open()
    try:
        for i in range(3):
            store.send("dev-a", "dev-b", f"m{i}", f"e{i}")
        rows = store.inbox("dev-b")
        assert [r["text"] for r in rows] == ["m1", "m2"]
    finally:
        store.close()


def test_presence_tracks_names_and_online_ids():
    presence = PigeonPresence()
    presence.touch("dev-a", "书房")
    presence.touch("dev-b", "")
    assert presence.is_online("dev-a")
    assert presence.name("dev-a") == "书房"
    assert presence.name("dev-b") == ""
    assert presence.online_ids(exclude="dev-a") == ["dev-b"]
    assert presence.is_online("dev-c") is False


def test_device_registry_persists_names_and_offline_devices(tmp_path):
    store = PigeonStore(tmp_path / "pigeon.db")
    store.open()
    try:
        store.touch_device("dev-a", "书房")
        store.touch_device("dev-b", "")
        store.touch_device("dev-a", "")
        rows = {row["id"]: row for row in store.devices(exclude_id="dev-a")}
        assert set(rows) == {"dev-b"}
        assert store.has_device("dev-a") is True
        assert store.has_device("dev-b") is True
        assert store.has_device("dev-c") is False
        # The empty heartbeat did not erase the earlier name.
        all_rows = {row["id"]: row for row in store.devices()}
        assert all_rows["dev-a"]["name"] == "书房"
    finally:
        store.close()


def test_device_registry_survives_reopen(tmp_path):
    db = tmp_path / "pigeon.db"
    store = PigeonStore(db)
    store.open()
    store.touch_device("dev-a", "书房")
    store.close()

    reopened = PigeonStore(db)
    reopened.open()
    try:
        assert reopened.has_device("dev-a") is True
        assert reopened.devices()[0]["name"] == "书房"
    finally:
        reopened.close()

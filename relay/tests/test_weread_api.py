"""WeRead bridge: device auth, compact responses, key config, upstream errors."""

from __future__ import annotations

import json
import os
import stat

import httpx
import pytest
from fastapi.testclient import TestClient

from relay import weread
from relay.config import reload_settings
from relay.main import app

_REAL_ASYNC_CLIENT = httpx.AsyncClient

A = {"Authorization": "Bearer token-a", "X-Device-Id": "dev-a"}
ADMIN = {"X-Admin-Token": "test-admin-token"}

SHELF = {
    "books": [
        {
            "bookId": "1001",
            "title": "小王子",
            "author": "圣埃克苏佩里",
            "readUpdateTime": 1700000000,
            "finishReading": 0,
            "isTop": 0,
            "readerLink": "https://weread.qq.com/web/reader/abc",
        },
        {
            "bookId": "1002",
            "title": "活着",
            "author": "余华",
            "readUpdateTime": 1600000000,
            "finishReading": 1,
            "isTop": 1,
            "readerLink": "https://weread.qq.com/web/reader/def",
        },
    ],
    "albums": [{"albumInfo": {"albumId": "a1", "name": "有声书"}}],
    "mp": {},
    "archive": [],
    "bookCount": 2,
}

PROGRESS = {
    "book": {
        "progress": 42,
        "recordReadingTime": 3600,
        "finishTime": 0,
        "chapterUid": 12,
        "chapterOffset": 100,
    }
}

STATS = {"totalReadTime": 7200, "readDays": 4, "dayAverageReadTime": 1800, "readStat": []}


def _client_factory_with(handler):
    transport = httpx.MockTransport(handler)
    def factory(**kwargs):
        kwargs["transport"] = transport
        return _REAL_ASYNC_CLIENT(**kwargs)

    return factory


def _handler(request: httpx.Request) -> httpx.Response:
    body = json.loads(request.content)
    assert request.headers["authorization"] == "Bearer wrk-test-key-1234567890"
    assert body["skill_version"] == "1.0.5"
    api = body["api_name"]
    if api == "/shelf/sync":
        return httpx.Response(200, json=SHELF)
    if api == "/book/getprogress":
        assert body["bookId"] == "1001"
        return httpx.Response(200, json=PROGRESS)
    if api == "/readdata/detail":
        assert body["mode"] == "overall"
        return httpx.Response(200, json=STATS)
    return httpx.Response(404, json={"errcode": -1, "errmsg": "unknown api"})


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("RELAY_WEREAD_DIR", str(tmp_path / "weread"))
    monkeypatch.setenv("WEREAD_API_KEY", "wrk-test-key-1234567890")
    monkeypatch.setenv("ALLOWED_DEVICE_IDS", "dev-a")
    monkeypatch.setenv("RELAY_DEVICE_TOKENS", "dev-a:token-a")
    monkeypatch.setenv("WEREAD_SHELF_TTL_S", "0")
    monkeypatch.setenv("WEREAD_PROGRESS_TTL_S", "0")
    monkeypatch.setenv("WEREAD_STATS_TTL_S", "0")
    monkeypatch.setattr(weread.httpx, "AsyncClient", _client_factory_with(_handler))
    reload_settings()
    with TestClient(app) as c:
        yield c
    reload_settings()


def test_requires_device_token(client):
    assert client.get("/api/v1/weread/status").status_code == 401
    assert client.get("/api/v1/weread/shelf").status_code == 401
    assert client.get("/api/v1/weread/book", params={"bookId": "1001"}).status_code == 401


def test_shelf_is_compact_and_pinned_first(client):
    response = client.get("/api/v1/weread/shelf", headers=A)
    assert response.status_code == 200
    body = response.json()
    assert body["total"] == 2
    assert [book["bookId"] for book in body["books"]] == ["1002", "1001"]
    assert body["books"][0]["finished"] is True
    assert "readerLink" not in body["books"][0]
    assert "cover" not in body["books"][0]


def test_book_progress_and_reader_link(client):
    response = client.get("/api/v1/weread/book", headers=A, params={"bookId": "1001"})
    assert response.status_code == 200
    body = response.json()
    assert body["title"] == "小王子"
    assert body["progress"] == 42
    assert body["readingTime"] == 3600
    assert body["readerLink"].startswith("https://weread.qq.com/")


def test_stats(client):
    response = client.get("/api/v1/weread/stats", headers=A, params={"mode": "overall"})
    assert response.status_code == 200
    assert response.json()["totalReadTime"] == 7200
    assert response.json()["readDays"] == 4


def test_upstream_401_maps_to_key_error(client, monkeypatch):
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(401, json={"errcode": -2013, "errmsg": "鉴权失败"})

    monkeypatch.setattr(weread.httpx, "AsyncClient", _client_factory_with(handler))
    response = client.get("/api/v1/weread/shelf", headers=A)
    assert response.status_code == 502
    assert response.json() == {"ok": False, "error": "weread_auth"}


def test_admin_saves_key_to_private_file_without_echoing_it(tmp_path, monkeypatch):
    monkeypatch.delenv("WEREAD_API_KEY", raising=False)
    monkeypatch.setenv("RELAY_WEREAD_DIR", str(tmp_path / "weread-admin"))
    monkeypatch.setenv("ALLOWED_DEVICE_IDS", "dev-a")
    monkeypatch.setenv("RELAY_DEVICE_TOKENS", "dev-a:token-a")
    reload_settings()
    with TestClient(app) as c:
        before = c.get("/admin/weread", headers=ADMIN).json()
        assert before["configured"] is False

        saved = c.post(
            "/admin/weread",
            headers={**ADMIN, "Content-Type": "application/json"},
            json={"apiKey": "wrk-admin-saved-1234567890"},
        )
        assert saved.status_code == 200
        body = saved.json()
        assert body["configured"] is True
        assert body["source"] == "file"
        assert "wrk-admin-saved" not in saved.text

        status = c.get("/api/v1/weread/status", headers=A).json()
        assert status["configured"] is True
        assert status["source"] == "file"

        key_file = tmp_path / "weread-admin" / "api_key"
        assert key_file.read_text(encoding="utf-8").strip() == "wrk-admin-saved-1234567890"
        if os.name == "posix":
            assert stat.S_IMODE(key_file.stat().st_mode) == 0o600

        cleared = c.delete("/admin/weread", headers=ADMIN)
        assert cleared.status_code == 200
        assert cleared.json()["configured"] is False
        assert not key_file.exists()
    reload_settings()


def test_project_root_key_file_fallback(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    (tmp_path / "微信读书key").write_text("wrk-test-key-1234567890\n", encoding="utf-8")
    settings = weread.Settings(RELAY_ENV="development", RELAY_WEREAD_DIR=str(tmp_path / "run"))
    store = weread.WeReadStore(settings)
    assert store.configured is True
    assert store.source() == "project_file"

"""Text-to-image cards: pipeline, assets endpoints, and BMP format guarantees.

The BMP assertions mirror the firmware's Bitmap reader constraints
(lib/GfxRenderer/BitmapHelpers.cpp): 1 bpp, 62-byte header+palette, rows
padded to 4 bytes — at 480x800 exactly 48,062 bytes, one framebuffer.
"""

from __future__ import annotations

import asyncio
import base64
import io
import json
import os
import time
from pathlib import Path

import httpx
import pytest
from fastapi.testclient import TestClient
from PIL import Image

from relay import t2i
from relay.config import get_settings, reload_settings
from relay.main import app

DEVICE = {"Authorization": "Bearer test-device-token", "X-Device-Id": "debug-client"}

PANEL_BYTES = 62 + 480 * 800 // 8  # 48,062


def bmp_fields(data: bytes) -> dict:
    assert data[:2] == b"BM"
    return {
        "file_size": int.from_bytes(data[2:6], "little"),
        "pixel_offset": int.from_bytes(data[10:14], "little"),
        "header_size": int.from_bytes(data[14:18], "little"),
        "width": int.from_bytes(data[18:22], "little"),
        "height": int.from_bytes(data[22:26], "little"),
        "bpp": int.from_bytes(data[28:30], "little"),
        "colors_used": int.from_bytes(data[46:50], "little"),
    }


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("T2I_PROVIDER", "mock")
    monkeypatch.setenv("T2I_MOCK_DELAY_S", "0")
    monkeypatch.setenv("T2I_ASSET_DIR", str(tmp_path / "assets"))
    reload_settings()
    with TestClient(app) as c:
        yield c
    monkeypatch.undo()
    reload_settings()


def test_mock_generate_and_fetch(client):
    response = client.post("/api/v1/image", json={"text": "山间小屋，炊烟袅袅"}, headers=DEVICE)
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["text"] in t2i.WAITING_PHRASES
    image = body["image"]
    assert t2i.ASSET_ID_RE.match(image["id"])
    assert image["url"] == f"/api/v1/image/{image['id']}"
    assert (image["width"], image["height"], image["bytes"], image["format"]) == (480, 800, PANEL_BYTES, "bmp")

    fetched = client.get(image["url"], headers=DEVICE)
    assert fetched.status_code == 200
    assert fetched.headers["content-type"] == "image/bmp"
    data = fetched.content
    assert len(data) == image["bytes"] == PANEL_BYTES

    fields = bmp_fields(data)
    assert fields["file_size"] == PANEL_BYTES
    assert fields["pixel_offset"] == 62
    assert fields["header_size"] == 40
    assert (fields["width"], abs(fields["height"])) == (480, 800)
    assert fields["bpp"] == 1
    assert fields["colors_used"] <= 2
    # A dithered image must actually use both ink values.
    pixels = data[62:]
    assert b"\x00" in pixels and b"\xff" in pixels


def test_mock_is_deterministic_per_prompt(client):
    first = client.post("/api/v1/image", json={"text": "同一条提示词"}, headers=DEVICE).json()
    second = client.post("/api/v1/image", json={"text": "同一条提示词"}, headers=DEVICE).json()
    one = client.get(first["image"]["url"], headers=DEVICE).content
    two = client.get(second["image"]["url"], headers=DEVICE).content
    assert one == two


def test_asset_and_generate_require_auth(client):
    assert client.post("/api/v1/image", json={"text": "x"}).status_code == 401
    assert client.get("/api/v1/image/00000000000000ff").status_code == 401
    bad = {"Authorization": "Bearer wrong-token", "X-Device-Id": "debug-client"}
    assert client.post("/api/v1/image", json={"text": "x"}, headers=bad).status_code == 401


def test_unknown_or_malformed_asset_is_404(client):
    assert client.get("/api/v1/image/00000000000000ff", headers=DEVICE).status_code == 404
    assert client.get("/api/v1/image/not-an-id", headers=DEVICE).status_code == 404
    traversal = client.get("/api/v1/image/..%2f..%2fvoice-relay.env", headers=DEVICE)
    assert traversal.status_code == 404


def test_empty_prompt_rejected(client):
    response = client.post("/api/v1/image", json={"text": "   "}, headers=DEVICE)
    assert response.status_code == 400
    assert response.json()["code"] == "T2I_EMPTY_PROMPT"


def test_dashscope_without_key_fails_closed(client, monkeypatch):
    monkeypatch.setenv("T2I_PROVIDER", "dashscope")
    monkeypatch.delenv("DASHSCOPE_API_KEY", raising=False)
    reload_settings()
    response = client.post("/api/v1/image", json={"text": "海边日落"}, headers=DEVICE)
    assert response.status_code == 502
    assert response.json()["code"] == "T2I_NOT_CONFIGURED"


def test_cleanup_drops_expired_assets(client, monkeypatch):
    settings = get_settings()
    fresh = t2i.save_asset(settings, b"BMfresh")
    stale = t2i.save_asset(settings, b"BMstale")
    asset_dir = Path(settings.t2i_asset_dir)
    old = time.time() - 2 * settings.t2i_asset_ttl_s
    os.utime(asset_dir / f"{stale}.bmp", (old, old))

    removed = t2i.cleanup_assets(settings)
    assert removed == 1
    assert (asset_dir / f"{fresh}.bmp").exists()
    assert not (asset_dir / f"{stale}.bmp").exists()


def test_to_bmp_fits_arbitrary_source():
    source = Image.linear_gradient("L").rotate(30, fillcolor=255).resize((1600, 601))
    data = t2i.to_bmp(source, 800, 480)
    fields = bmp_fields(data)
    assert (fields["width"], fields["height"], fields["bpp"]) == (800, 480, 1)
    assert fields["pixel_offset"] == 62
    assert len(data) == PANEL_BYTES


# --- dashscope provider against a mocked transport --------------------------


def _client_factory_with(handler) -> callable:
    transport = httpx.MockTransport(handler)
    real_client = httpx.AsyncClient  # captured before the monkeypatch

    def factory(**kwargs):
        kwargs["transport"] = transport
        return real_client(**kwargs)

    return factory


@pytest.fixture()
def dashscope_env(tmp_path, monkeypatch):
    monkeypatch.setenv("T2I_PROVIDER", "dashscope")
    monkeypatch.setenv("DASHSCOPE_API_KEY", "test-key")
    monkeypatch.setenv("T2I_POLL_INTERVAL_S", "0")
    monkeypatch.setenv("T2I_ASSET_DIR", str(tmp_path / "assets"))
    reload_settings()
    yield
    monkeypatch.undo()
    reload_settings()


def test_dashscope_sends_async_header_only_on_submit(monkeypatch, dashscope_env):
    seen: list[tuple[str, str, dict]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        seen.append((request.method, request.url.path, dict(request.headers)))
        if request.method == "POST":
            return httpx.Response(200, json={"output": {"task_id": "task-1"}})
        if request.url.host == "oss.example":
            buf = io.BytesIO()
            Image.new("L", (320, 240), 200).save(buf, format="PNG")
            return httpx.Response(200, content=buf.getvalue())
        return httpx.Response(
            200,
            json={"output": {"task_status": "SUCCEEDED", "results": [{"url": "https://oss.example/a.png"}]}},
        )

    monkeypatch.setattr(t2i.httpx, "AsyncClient", _client_factory_with(handler))
    data = asyncio.run(t2i.render(get_settings(), "海边日落"))
    assert len(data) == PANEL_BYTES

    submit_headers = [h for (m, p, h) in seen if m == "POST"]
    poll_headers = [h for (m, p, h) in seen if m == "GET" and p.endswith("/tasks/task-1")]
    assert submit_headers[0].get("x-dashscope-async") == "enable"
    assert poll_headers, "the task must be polled at least once"
    assert all("x-dashscope-async" not in h for h in poll_headers)


def test_dashscope_persistent_4xx_fails_fast(monkeypatch, dashscope_env):
    def handler(request: httpx.Request) -> httpx.Response:
        if request.method == "POST":
            return httpx.Response(200, json={"output": {"task_id": "task-2"}})
        return httpx.Response(403, json={"code": "AccessDenied", "message": "async not supported on GET"})

    monkeypatch.setattr(t2i.httpx, "AsyncClient", _client_factory_with(handler))
    with pytest.raises(t2i.T2iError) as excinfo:
        asyncio.run(t2i.render(get_settings(), "测试"))
    assert excinfo.value.code == "T2I_UPSTREAM"
    assert "403" in excinfo.value.detail


# --- openai-compatible provider against a mocked transport ------------------


@pytest.fixture()
def openai_env(tmp_path, monkeypatch):
    monkeypatch.setenv("T2I_PROVIDER", "openai")
    monkeypatch.setenv("T2I_BASE_URL", "https://images.example/v1")
    monkeypatch.setenv("T2I_API_KEY", "test-key")
    monkeypatch.setenv("T2I_MODEL", "gpt-image-1")
    monkeypatch.setenv("T2I_ASSET_DIR", str(tmp_path / "assets"))
    reload_settings()
    yield
    monkeypatch.undo()
    reload_settings()


def test_openai_without_config_fails_closed(client, tmp_path, monkeypatch):
    monkeypatch.setenv("T2I_PROVIDER", "openai")
    monkeypatch.delenv("T2I_BASE_URL", raising=False)
    monkeypatch.delenv("T2I_API_KEY", raising=False)
    monkeypatch.setenv("T2I_ASSET_DIR", str(tmp_path / "assets"))
    reload_settings()
    response = client.post("/api/v1/image", json={"text": "夜空"}, headers=DEVICE)
    assert response.status_code == 502
    assert response.json()["code"] == "T2I_NOT_CONFIGURED"


def test_openai_b64_json_success(monkeypatch, openai_env):
    seen: list[dict] = []

    def handler(request: httpx.Request) -> httpx.Response:
        assert request.url.path == "/v1/images/generations"
        seen.append({"headers": dict(request.headers), "json": json.loads(request.content)})
        buf = io.BytesIO()
        Image.linear_gradient("L").resize((640, 1024)).save(buf, format="PNG")
        return httpx.Response(200, json={"data": [{"b64_json": base64.b64encode(buf.getvalue()).decode()}]})

    monkeypatch.setattr(t2i.httpx, "AsyncClient", _client_factory_with(handler))
    data = asyncio.run(t2i.render(get_settings(), "山间小屋"))
    assert len(data) == PANEL_BYTES
    assert bmp_fields(data)["width"] == 480

    request = seen[0]
    assert request["headers"].get("authorization") == "Bearer test-key"
    assert request["json"]["model"] == "gpt-image-1"
    assert request["json"]["size"] == "768x1280"  # W*H converted to WxH
    assert request["json"]["response_format"] == "b64_json"


def test_openai_url_response_downloads(monkeypatch, openai_env):
    calls: list[str] = []

    def handler(request: httpx.Request) -> httpx.Response:
        calls.append(str(request.url))
        if request.url.host == "cdn.example":
            buf = io.BytesIO()
            Image.linear_gradient("L").resize((400, 600)).save(buf, format="PNG")
            return httpx.Response(200, content=buf.getvalue())
        return httpx.Response(200, json={"data": [{"url": "https://cdn.example/x.png"}]})

    monkeypatch.setattr(t2i.httpx, "AsyncClient", _client_factory_with(handler))
    data = asyncio.run(t2i.render(get_settings(), "海边日落"))
    assert len(data) == PANEL_BYTES
    assert any("cdn.example" in c for c in calls)


def test_openai_content_filter_maps_to_rejected(monkeypatch, openai_env):
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(
            400,
            json={"error": {"code": "content_policy_violation", "message": "Your request was rejected"}},
        )

    monkeypatch.setattr(t2i.httpx, "AsyncClient", _client_factory_with(handler))
    with pytest.raises(t2i.T2iError) as excinfo:
        asyncio.run(t2i.render(get_settings(), "测试"))
    assert excinfo.value.code == "T2I_REJECTED"


def test_openai_upstream_error_is_502(monkeypatch, openai_env):
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(500, text="boom")

    monkeypatch.setattr(t2i.httpx, "AsyncClient", _client_factory_with(handler))
    with pytest.raises(t2i.T2iError) as excinfo:
        asyncio.run(t2i.render(get_settings(), "测试"))
    assert excinfo.value.code == "T2I_UPSTREAM"


def test_openai_vendor_endpoint(monkeypatch, openai_env):
    """A gateway without the OpenAI /v1 prefix still works (base as given)."""
    monkeypatch.setenv("T2I_BASE_URL", "https://images.example/api/openai")
    reload_settings()
    seen: list[str] = []

    def handler(request: httpx.Request) -> httpx.Response:
        seen.append(request.url.path)
        buf = io.BytesIO()
        Image.new("L", (320, 320), 128).save(buf, format="PNG")
        return httpx.Response(200, json={"data": [{"b64_json": base64.b64encode(buf.getvalue()).decode()}]})

    monkeypatch.setattr(t2i.httpx, "AsyncClient", _client_factory_with(handler))
    data = asyncio.run(t2i.render(get_settings(), "测试"))
    assert len(data) == PANEL_BYTES
    assert seen == ["/api/openai/images/generations"]


# --- style / size / edit (image) parameters ----------------------------------


def _tiny_bmp_b64(size=(24, 16), fill=128) -> str:
    buf = io.BytesIO()
    Image.new("L", size, fill).save(buf, format="BMP")
    return base64.b64encode(buf.getvalue()).decode()


def test_style_and_size_reach_dashscope_submit(monkeypatch, dashscope_env):
    submits: list[dict] = []

    def handler(request: httpx.Request) -> httpx.Response:
        if request.method == "POST":
            submits.append(json.loads(request.content))
            return httpx.Response(200, json={"output": {"task_id": "task-style"}})
        if request.url.host == "oss.example":
            buf = io.BytesIO()
            Image.new("L", (320, 240), 200).save(buf, format="PNG")
            return httpx.Response(200, content=buf.getvalue())
        return httpx.Response(
            200,
            json={"output": {"task_status": "SUCCEEDED", "results": [{"url": "https://oss.example/a.png"}]}},
        )

    monkeypatch.setattr(t2i.httpx, "AsyncClient", _client_factory_with(handler))
    data = asyncio.run(t2i.render(get_settings(), "海边日落", style="sketch", size="512*768"))
    assert len(data) == PANEL_BYTES

    params = submits[0]["parameters"]
    assert params["style"] == "sketch"
    assert params["size"] == "512*768"


def test_unknown_style_and_malformed_size_rejected(client):
    bad_style = client.post("/api/v1/image", json={"text": "海边", "style": "vangogh"}, headers=DEVICE)
    assert bad_style.status_code == 400
    assert bad_style.json()["code"] == "T2I_BAD_PARAM"

    bad_size = client.post("/api/v1/image", json={"text": "海边", "size": "huge"}, headers=DEVICE)
    assert bad_size.status_code == 400
    assert bad_size.json()["code"] == "T2I_BAD_PARAM"


def test_mock_edit_with_uploaded_image_ok(client):
    response = client.post(
        "/api/v1/image",
        json={"text": "改成夜景", "image": {"data": _tiny_bmp_b64(), "format": "bmp", "width": 24, "height": 16}},
        headers=DEVICE,
    )
    assert response.status_code == 200, response.text
    assert (response.json()["image"]["width"], response.json()["image"]["height"]) == (480, 800)


def test_edit_bad_base64_and_missing_data_rejected(client):
    bad = client.post("/api/v1/image", json={"text": "x", "image": {"data": "!!not base64!!"}}, headers=DEVICE)
    assert bad.status_code == 400
    assert bad.json()["code"] == "T2I_BAD_PARAM"

    missing = client.post("/api/v1/image", json={"text": "x", "image": {"format": "bmp"}}, headers=DEVICE)
    assert missing.status_code == 400
    assert missing.json()["code"] == "T2I_BAD_PARAM"


def test_dashscope_edit_uses_image2image_route(monkeypatch, dashscope_env):
    submits: list[dict] = []
    paths: list[str] = []

    def handler(request: httpx.Request) -> httpx.Response:
        paths.append(request.url.path)
        if request.method == "POST":
            submits.append(json.loads(request.content))
            return httpx.Response(200, json={"output": {"task_id": "task-edit"}})
        if request.url.host == "oss.example":
            buf = io.BytesIO()
            Image.new("L", (320, 240), 200).save(buf, format="PNG")
            return httpx.Response(200, content=buf.getvalue())
        return httpx.Response(
            200,
            json={"output": {"task_status": "SUCCEEDED", "results": [{"url": "https://oss.example/a.png"}]}},
        )

    monkeypatch.setattr(t2i.httpx, "AsyncClient", _client_factory_with(handler))
    data = asyncio.run(
        t2i.render(get_settings(), "改成夜景", image={"data": _tiny_bmp_b64(), "format": "bmp"})
    )
    assert len(data) == PANEL_BYTES

    assert paths[0].endswith("/services/aigc/image2image/image-synthesis")
    payload = submits[0]
    assert payload["model"] == "wanx2.1-imageedit"
    assert payload["input"]["function"] == "description_edit"
    base_url = payload["input"]["base_image_url"]
    assert base_url.startswith("data:image/png;base64,")
    assert payload["input"]["prompt"] == "改成夜景"


def test_dashscope_edit_without_key_fails_closed(monkeypatch, dashscope_env):
    monkeypatch.delenv("DASHSCOPE_API_KEY", raising=False)
    reload_settings()
    with pytest.raises(t2i.T2iError) as excinfo:
        asyncio.run(t2i.render(get_settings(), "x", image={"data": _tiny_bmp_b64()}))
    assert excinfo.value.code == "T2I_NOT_CONFIGURED"


def test_openai_size_override_in_generations(monkeypatch, openai_env):
    seen: list[dict] = []

    def handler(request: httpx.Request) -> httpx.Response:
        seen.append(json.loads(request.content))
        buf = io.BytesIO()
        Image.new("L", (320, 320), 128).save(buf, format="PNG")
        return httpx.Response(200, json={"data": [{"b64_json": base64.b64encode(buf.getvalue()).decode()}]})

    monkeypatch.setattr(t2i.httpx, "AsyncClient", _client_factory_with(handler))
    data = asyncio.run(t2i.render(get_settings(), "山间小屋", size="640*1024"))
    assert len(data) == PANEL_BYTES
    assert seen[0]["size"] == "640x1024"


def test_openai_edit_uses_edits_endpoint(monkeypatch, openai_env):
    seen: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        seen.append(request)
        buf = io.BytesIO()
        Image.new("L", (320, 320), 128).save(buf, format="PNG")
        return httpx.Response(200, json={"data": [{"b64_json": base64.b64encode(buf.getvalue()).decode()}]})

    monkeypatch.setattr(t2i.httpx, "AsyncClient", _client_factory_with(handler))
    data = asyncio.run(t2i.render(get_settings(), "改成夜景", image={"data": _tiny_bmp_b64()}))
    assert len(data) == PANEL_BYTES

    request = seen[0]
    assert request.url.path.endswith("/images/edits")
    assert "multipart/form-data" in request.headers.get("content-type", "")
    body = request.content
    assert b'name="image"' in body and b"source.png" in body

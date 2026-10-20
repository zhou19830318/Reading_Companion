"""Text-to-image cards for the reader screen.

Pipeline (all server side):
    prompt -> provider image (mock: drawn locally, no cloud call;
              dashscope: wanx task submit + poll + download;
              openai: one-shot POST to an OpenAI-compatible images endpoint)
           -> fit to the panel size -> Floyd-Steinberg 1-bit BMP

The BMP matches what the firmware's Bitmap reader accepts
(lib/GfxRenderer/BitmapHelpers.cpp createBmpHeader): 1 bpp, two-entry
palette (0 = black, 1 = white), 4-byte padded rows. At 800x480 that is
exactly 48,062 bytes — one panel framebuffer row-for-row.

T2I_PROVIDER=mock is the default so the whole path (endpoint, conversion,
asset serving) is testable before any DashScope image quota is spent.
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import io
import logging
import os
import random
import re
import secrets
import time
from pathlib import Path

import httpx
from PIL import Image, ImageDraw, ImageFont, ImageOps

from .config import Settings

log = logging.getLogger(__name__)

__all__ = [
    "T2iError",
    "WAITING_PHRASES",
    "DASHSCOPE_STYLES",
    "pick_phrase",
    "render",
    "to_bmp",
    "asset_dir",
    "save_asset",
    "load_asset",
    "cleanup_assets",
]

# wanx V1 text-to-image style enum (DashScope docs). Keys are the API values,
# kept lowercase; the device sends exactly these. "auto" is the provider
# default and is accepted so the device can send an explicit reset.
DASHSCOPE_STYLES: frozenset[str] = frozenset(
    {
        "auto",
        "photo",
        "portrait",
        "3d cartoon",
        "anime",
        "oil",
        "watercolor",
        "sketch",
        "chinese_ink",
        "flat illustration",
    }
)

_SIZE_RE = re.compile(r"^\d{2,4}\*\d{2,4}$")
# Device BMPs are one panel framebuffer (~48 KB); the cap only exists to keep
# a broken/hostile client from asking the server to hold a huge decode.
_EDIT_SOURCE_MAX_BYTES = 8 * 1024 * 1024

# Waiting-screen phrases. The device shows one while the HTTP round trip is
# in flight (it cannot receive server text mid-call), so these are also part
# of the response for the SSE/streaming path and clients that want to chat.
WAITING_PHRASES: tuple[str, ...] = (
    "正在铺开画纸……",
    "正在研墨，马上起笔……",
    "画师正在起稿……",
    "正在勾勒轮廓……",
    "正在上色，稍等片刻……",
    "正在给画面添上最后一笔……",
    "墨迹将干，马上就好……",
)

ASSET_ID_RE = re.compile(r"^[0-9a-f]{16}$")

# Optional CJK fonts for the mock caption; absent on a bare server, in which
# case the caption falls back to ASCII.
_CJK_FONT_CANDIDATES = (
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/arphic/uming.ttc",
)


class T2iError(Exception):
    """Generation failure with a stable device-facing code string."""

    def __init__(self, code: str, detail: str = ""):
        super().__init__(detail or code)
        self.code = code
        self.detail = detail


def pick_phrase(rng: random.Random | None = None) -> str:
    return (rng or random).choice(WAITING_PHRASES)


# ---------------------------------------------------------------------------
# generation
# ---------------------------------------------------------------------------


async def render(
    settings: Settings,
    prompt: str,
    style: str = "",
    size: str = "",
    image: dict | None = None,
) -> bytes:
    """Run the full pipeline and return the 1-bit BMP bytes.

    style: optional DashScope V1 style (see DASHSCOPE_STYLES); ignored by
        providers without a style concept, rejected if not in the whitelist.
    size: optional provider size override "W*H"; the panel conversion at the
        end always uses T2I_WIDTH/T2I_HEIGHT.
    image: optional source image for edit mode — {"data": <base64>,
        "format": "bmp"?, "width"?: int, "height"?: int}. When present the
        provider runs image-to-image on the uploaded picture instead of a
        pure text-to-image render.
    """
    prompt = (prompt or "").strip()
    if not prompt:
        raise T2iError("T2I_EMPTY_PROMPT", "prompt is empty")

    style = (style or "").strip()
    if style and style not in DASHSCOPE_STYLES:
        raise T2iError("T2I_BAD_PARAM", f"unknown style {style!r}")
    size = (size or "").strip()
    if size and not _SIZE_RE.match(size):
        raise T2iError("T2I_BAD_PARAM", f"malformed size {size!r}")
    source = _decode_edit_source(image) if image else None

    if settings.t2i_provider == "mock":
        # Keep a little latency so timeouts/polling behavior is exercised.
        if settings.t2i_mock_delay_s > 0:
            await asyncio.sleep(settings.t2i_mock_delay_s)
        image_out = _mock_image(prompt, settings.t2i_width, settings.t2i_height, source=source)
    elif settings.t2i_provider == "dashscope":
        if source is not None:
            image_out = await _render_dashscope_edit(settings, prompt, source)
        else:
            image_out = await _render_dashscope(settings, prompt, style, size)
    elif settings.t2i_provider == "openai":
        if source is not None:
            image_out = await _render_openai_edit(settings, prompt, source)
        else:
            image_out = await _render_openai(settings, prompt, size)
    else:
        raise T2iError("T2I_NOT_CONFIGURED", f"unknown T2I_PROVIDER {settings.t2i_provider!r}")

    return to_bmp(image_out, settings.t2i_width, settings.t2i_height)


def _decode_edit_source(image: dict) -> Image.Image:
    """Decode the device's uploaded edit source (base64 BMP) to a PIL image."""
    data_b64 = image.get("data") if isinstance(image, dict) else None
    if not isinstance(data_b64, str) or not data_b64:
        raise T2iError("T2I_BAD_PARAM", "image.data missing")
    if len(data_b64) > _EDIT_SOURCE_MAX_BYTES:  # b64 expands ~4/3; cap the wire size
        raise T2iError("T2I_BAD_PARAM", "image too large")
    try:
        raw = base64.b64decode(data_b64)
    except (ValueError, TypeError) as exc:
        raise T2iError("T2I_BAD_PARAM", f"image.data is not base64: {exc}")
    if not raw:
        raise T2iError("T2I_BAD_PARAM", "image.data decoded to zero bytes")
    try:
        return Image.open(io.BytesIO(raw))
    except OSError as exc:
        raise T2iError("T2I_BAD_PARAM", f"image decode failed: {exc}")


def _to_png_b64(image: Image.Image, min_width: int = 0) -> str:
    """PNG is what both edit endpoints accept (device BMP is converted here).

    min_width: wanx2.1-imageedit rejects sources narrower than 512 px
    (verified live: "The width of the image should be between 512 and 4096
    pixels"); the panel BMP is 480 wide, so it is scaled up first.
    """
    img = ImageOps.exif_transpose(image).convert("RGB")
    if min_width and img.width < min_width:
        factor = min_width / img.width
        img = img.resize((min_width, max(1, round(img.height * factor))), Image.Resampling.LANCZOS)
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode()


def to_bmp(image: Image.Image, width: int, height: int) -> bytes:
    """Fit any source image to the panel and dither it to a 1-bit BMP."""
    img = ImageOps.exif_transpose(image)
    gray = ImageOps.fit(img.convert("L"), (width, height), method=Image.Resampling.LANCZOS)
    # E-ink has no true mid-tones; stretch first so the dither uses the full
    # black/white range instead of a washed-out histogram.
    gray = ImageOps.autocontrast(gray, cutoff=1)
    mono = gray.convert("1", dither=Image.Dither.FLOYDSTEINBERG)
    out = io.BytesIO()
    mono.save(out, format="BMP")
    return out.getvalue()


def _load_font(size: int) -> ImageFont.FreeTypeFont | None:
    for path in _CJK_FONT_CANDIDATES:
        if Path(path).exists():
            try:
                return ImageFont.truetype(path, size)
            except OSError:
                continue
    return None


def _mock_image(prompt: str, width: int, height: int, source: Image.Image | None = None) -> Image.Image:
    """A deterministic placeholder keyed off the prompt (same prompt -> same
    picture), shaped like a real generation: background, motif, caption.
    With `source` (edit mode) the uploaded picture is the background."""
    seed = int.from_bytes(hashlib.sha256(prompt.encode("utf-8")).digest()[:8], "big")
    rng = random.Random(seed)

    if source is not None:
        img = ImageOps.fit(ImageOps.exif_transpose(source).convert("L"), (width, height))
    else:
        img = Image.linear_gradient("L").rotate(rng.randint(0, 359), expand=False, fillcolor=255)
        img = img.resize((width, height), Image.Resampling.BILINEAR)
    draw = ImageDraw.Draw(img)

    cx, cy = width // 2, height // 2
    rings = rng.randint(3, 6)
    for i in range(rings):
        r = min(width, height) * (i + 1) // (rings * 2 + 2)
        draw.ellipse(
            (cx - r, cy - r, cx + r, cy + r),
            outline=rng.randint(40, 215),
            width=max(2, r // 24),
        )

    margin = min(width, height) // 24
    draw.rectangle((margin, margin, width - margin - 1, height - margin - 1), outline=40, width=4)

    font_size = max(20, height // 15)
    font = _load_font(font_size)
    label = prompt if font is not None else f"[MOCK] {seed:016x}"[:40]
    text_x = margin + font_size // 2
    text_y = height - margin - font_size * 2
    draw.text((text_x + 2, text_y + 2), label[:24], fill=200, font=font)  # soft shadow
    draw.text((text_x, text_y), label[:24], fill=10, font=font)
    return img


async def _render_dashscope(settings: Settings, prompt: str, style: str = "", size: str = "") -> Image.Image:
    """Submit a wanx text-to-image task, poll it to completion, download the result.

    DashScope text-to-image is asynchronous by contract: the submit call
    returns a task id and the image arrives later as an OSS URL (valid ~24 h,
    hence the immediate download).
    """
    if not settings.dashscope_api_key:
        raise T2iError("T2I_NOT_CONFIGURED", "DASHSCOPE_API_KEY is not set")

    parameters: dict = {
        "size": size or settings.t2i_size,
        "n": 1,
        # The panel is small; the default watermark would dominate it.
        "watermark": False,
    }
    if style:
        parameters["style"] = style
    payload = {
        "model": settings.t2i_model,
        "input": {"prompt": prompt},
        "parameters": parameters,
    }
    return await _dashscope_run_task(settings, "/services/aigc/text2image/image-synthesis", payload)


async def _render_dashscope_edit(settings: Settings, prompt: str, source: Image.Image) -> Image.Image:
    """wanx2.1-imageedit: same async task protocol as t2i, image2image route.

    base_image_url accepts a data: URI, so the device's panel BMP (converted
    to PNG here) travels inside the submit call — no server-side staging of
    uploads, no dependency on the caller keeping an asset around.
    """
    if not settings.dashscope_api_key:
        raise T2iError("T2I_NOT_CONFIGURED", "DASHSCOPE_API_KEY is not set")

    # `function` belongs in `input` and is mandatory (verified live: the task
    # fails with "The following fields are required: function" when it is
    # missing or sits in parameters). description_edit = free-form edit
    # driven by the text prompt — the device's 修改 flow.
    payload = {
        "model": settings.t2i_edit_model,
        "input": {
            "function": "description_edit",
            "base_image_url": f"data:image/png;base64,{_to_png_b64(source, min_width=512)}",
            "prompt": prompt,
        },
        "parameters": {"n": 1},
    }
    return await _dashscope_run_task(settings, "/services/aigc/image2image/image-synthesis", payload)


async def _dashscope_run_task(settings: Settings, submit_path: str, payload: dict) -> Image.Image:
    """Shared wanx async runner: submit -> poll -> download the OSS result."""
    base = settings.dashscope_t2i_base_url()
    auth_headers = {"Authorization": f"Bearer {settings.dashscope_api_key}"}
    # X-DashScope-Async is submit-only: sending it on the task-poll GET makes
    # DashScope answer 403 "current user api does not support asynchronous
    # calls" (verified on the cn-beijing MaaS workspace endpoint), which used
    # to be swallowed as a poll failure and ended in a 150 s timeout.
    submit_headers = {**auth_headers, "X-DashScope-Async": "enable"}

    async with httpx.AsyncClient(timeout=30.0) as client:
        submit = await client.post(f"{base}{submit_path}", json=payload, headers=submit_headers)
        if submit.status_code != 200:
            raise T2iError("T2I_UPSTREAM", f"submit HTTP {submit.status_code}: {submit.text[:200]}")
        task_id = (submit.json().get("output") or {}).get("task_id")
        if not task_id:
            raise T2iError("T2I_UPSTREAM", f"submit response has no task_id: {submit.text[:200]}")
        log.info("t2i task submitted", extra={"task_id": task_id, "model": payload.get("model")})

        deadline = time.monotonic() + settings.t2i_timeout_s
        consecutive_4xx = 0
        logged_failure = False
        while time.monotonic() < deadline:
            await asyncio.sleep(settings.t2i_poll_interval_s)
            poll = await client.get(f"{base}/tasks/{task_id}", headers=auth_headers)
            if poll.status_code != 200:
                detail = f"HTTP {poll.status_code}: {poll.text[:160]}"
                if not logged_failure:
                    logged_failure = True
                    log.warning("t2i poll failing", extra={"task_id": task_id, "detail": detail})
                # A persistent 4xx is a config/route problem (auth, wrong
                # workspace); fail early instead of burning the whole timeout.
                if 400 <= poll.status_code < 500:
                    consecutive_4xx += 1
                    if consecutive_4xx >= 2:
                        raise T2iError("T2I_UPSTREAM", f"poll {detail}")
                else:
                    consecutive_4xx = 0
                continue
            consecutive_4xx = 0
            output = poll.json().get("output") or {}
            status = output.get("task_status")
            if status == "SUCCEEDED":
                results = output.get("results") or []
                image_url = results[0].get("url") if results else None
                if not image_url:
                    raise T2iError("T2I_UPSTREAM", "task succeeded without an image url")
                download = await client.get(image_url, timeout=60.0)
                if download.status_code != 200:
                    raise T2iError("T2I_UPSTREAM", f"image download HTTP {download.status_code}")
                return Image.open(io.BytesIO(download.content))
            if status in ("FAILED", "CANCELED", "UNKNOWN"):
                code = str(output.get("code") or "")
                message = str(output.get("message") or status)
                # Content moderation rejections are a class of their own: the
                # device shows "try another wording" rather than a fault.
                if "Inspection" in code or "inspection" in message or "内容" in message:
                    raise T2iError("T2I_REJECTED", message[:200])
                raise T2iError("T2I_UPSTREAM", f"{code or status}: {message[:180]}")

        raise T2iError("T2I_TIMEOUT", f"task not finished within {settings.t2i_timeout_s:.0f}s")


async def _render_openai(settings: Settings, prompt: str, size: str = "") -> Image.Image:
    """One-shot POST to an OpenAI-compatible images endpoint.

    Sync by contract (unlike DashScope): the response carries the image
    inline as base64 (preferred — no second hop, no expiring URL) or as a
    URL to download. The request size is OpenAI's WxH; T2I_SIZE is W*H.
    """
    if not settings.t2i_api_key or not settings.t2i_base_url:
        raise T2iError("T2I_NOT_CONFIGURED", "T2I_BASE_URL / T2I_API_KEY are not set")

    url = settings.t2i_base_url.rstrip("/") + "/images/generations"
    payload = {
        "model": settings.t2i_model,
        "prompt": prompt,
        "size": (size or settings.t2i_size).replace("*", "x"),
        "n": 1,
        "response_format": "b64_json",
    }
    headers = {"Authorization": f"Bearer {settings.t2i_api_key}"}

    async with httpx.AsyncClient(timeout=settings.t2i_timeout_s) as client:
        response = await client.post(url, json=payload, headers=headers)
        return await _parse_openai_images_response(response)


async def _render_openai_edit(settings: Settings, prompt: str, source: Image.Image) -> Image.Image:
    """POST /images/edits with the uploaded picture as multipart `image`.

    gpt-image-1 wants PNG/JPEG/WebP — the device BMP is converted here.
    response_format is omitted: gpt-image-1 edits always answer b64_json and
    some gateways reject the parameter outright.
    """
    if not settings.t2i_api_key or not settings.t2i_base_url:
        raise T2iError("T2I_NOT_CONFIGURED", "T2I_BASE_URL / T2I_API_KEY are not set")

    url = settings.t2i_base_url.rstrip("/") + "/images/edits"
    png = io.BytesIO()
    ImageOps.exif_transpose(source).convert("RGB").save(png, format="PNG")
    headers = {"Authorization": f"Bearer {settings.t2i_api_key}"}

    async with httpx.AsyncClient(timeout=settings.t2i_timeout_s) as client:
        response = await client.post(
            url,
            data={"model": settings.t2i_model, "prompt": prompt, "n": "1"},
            files={"image": ("source.png", png.getvalue(), "image/png")},
            headers=headers,
        )
        return await _parse_openai_images_response(response)


async def _parse_openai_images_response(response: httpx.Response) -> Image.Image:
    if response.status_code != 200:
        detail = response.text[:200]
        lower = detail.lower()
        # Content filters are a class of their own (the device shows
        # "try another wording" instead of a fault); OpenAI answers
        # 400 with code content_policy_violation, gateways vary.
        if response.status_code in (400, 403, 422) and (
            "content_policy" in lower or "moderation" in lower or "safety" in lower or "内容" in detail
        ):
            raise T2iError("T2I_REJECTED", detail)
        raise T2iError("T2I_UPSTREAM", f"HTTP {response.status_code}: {detail}")

    data = response.json().get("data") or []
    if not data:
        raise T2iError("T2I_UPSTREAM", f"response has no data: {response.text[:160]}")
    first = data[0] or {}

    b64 = first.get("b64_json")
    if b64:
        try:
            return Image.open(io.BytesIO(base64.b64decode(b64)))
        except (ValueError, OSError) as exc:
            raise T2iError("T2I_UPSTREAM", f"b64 image decode failed: {exc}")

    image_url = first.get("url")
    if image_url:
        return await _openai_download(image_url)

    raise T2iError("T2I_UPSTREAM", f"response has neither b64_json nor url: {response.text[:160]}")


async def _openai_download(image_url: str) -> Image.Image:
    async with httpx.AsyncClient(timeout=60.0) as client:
        download = await client.get(image_url)
        if download.status_code != 200:
            raise T2iError("T2I_UPSTREAM", f"image download HTTP {download.status_code}")
        return Image.open(io.BytesIO(download.content))


# ---------------------------------------------------------------------------
# asset store (served back to the device by GET /api/v1/image/{id})
# ---------------------------------------------------------------------------


def _resolve_asset_dir(settings: Settings) -> Path:
    base = settings.t2i_asset_dir.strip() or os.path.join("run", "t2i-assets")
    return Path(base)


def asset_dir(settings: Settings) -> Path:
    path = _resolve_asset_dir(settings)
    path.mkdir(parents=True, exist_ok=True)
    return path


def save_asset(settings: Settings, data: bytes) -> str:
    cleanup_assets(settings)
    asset_id = secrets.token_hex(8)
    (asset_dir(settings) / f"{asset_id}.bmp").write_bytes(data)
    return asset_id


def load_asset(settings: Settings, asset_id: str) -> bytes | None:
    if not ASSET_ID_RE.match(asset_id):
        return None
    try:
        return (_resolve_asset_dir(settings) / f"{asset_id}.bmp").read_bytes()
    except FileNotFoundError:
        return None


def cleanup_assets(settings: Settings, now: float | None = None) -> int:
    """Drop asset files older than T2I_ASSET_TTL_S. Returns how many went."""
    ttl = settings.t2i_asset_ttl_s
    if ttl <= 0:
        return 0
    now = time.time() if now is None else now
    removed = 0
    try:
        candidates = list(_resolve_asset_dir(settings).glob("*.bmp"))
    except OSError:
        return 0
    for path in candidates:
        try:
            if now - path.stat().st_mtime > ttl:
                path.unlink()
                removed += 1
        except OSError:
            continue
    return removed

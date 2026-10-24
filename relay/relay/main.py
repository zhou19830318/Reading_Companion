"""Voice Relay HTTP/WebSocket application.

Routes (docs section 17.5):
  GET  /healthz                 liveness probe, no auth, no upstream checks
  GET  /readyz                  config readiness, no auth
  GET  /admin                   setup page, host-only (loopback) + admin token
  GET  /admin/status            JSON backing the setup page
  GET  /api/v1/device/config    device capabilities, device token
  WSS  /api/v1/voice/stream     the device's realtime channel
  POST /api/v1/chat             admin-only text debug (SSE or JSON)
  GET  /metrics                 prometheus, internal exposure only
"""

from __future__ import annotations

import asyncio
import logging
import time
from contextlib import asynccontextmanager

import orjson
from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse, Response, StreamingResponse
from prometheus_client import CONTENT_TYPE_LATEST, generate_latest
from pydantic import BaseModel

from . import SERVICE_NAME, __version__
from . import admin_page, dict_source, learning_store, metrics, protocol, t2i
from .auth import check_admin, check_device
from .config import Settings, get_settings
from .logging_setup import setup_logging
from .protocol import ErrorCode, ProtocolError, parse_device_event
from .providers import contract_report, make_llm_provider
from .providers.base import ProviderError
from .session import VoiceSession

log = logging.getLogger("relay.main")

WS_CLOSE_UNAUTHORIZED = 4401
WS_CLOSE_FORBIDDEN = 4403
WS_CLOSE_PROTOCOL = 4400

OUTBOUND_QUEUE_MAX = 512
AUTH_GRACE_MESSAGES = 8  # event frames accepted before session.start


@asynccontextmanager
async def lifespan(app: FastAPI):
    settings = get_settings()
    setup_logging(settings.relay_log_level)
    log.info(
        "voice relay starting",
        extra={
            "service": SERVICE_NAME,
            "version": settings.relay_version,
            "env": settings.relay_env,
            "asr_provider": settings.asr_provider,
            "llm_provider": settings.llm_provider,
            "contracts": contract_report(settings),
        },
    )
    app.state.sessions = set()
    app.state.ws_connections = 0
    # Vocabulary learning: one SQLite connection for the process (schema is
    # created idempotently; the directory must exist before the unit's
    # sandboxed write happens, which LearningStore handles).
    learning = learning_store.LearningStore(settings.learning_db_path)
    learning.open()
    app.state.learning = learning
    log.info("learning store ready", extra={"db": str(learning.db_path)})
    yield
    # Graceful shutdown: stop accepting, cancel upstream work in bounded time.
    sessions: set[VoiceSession] = app.state.sessions
    if sessions:
        log.info("cancelling sessions at shutdown", extra={"count": len(sessions)})
        await asyncio.gather(*(s.abort("server_shutdown") for s in list(sessions)), return_exceptions=True)
    learning.close()


app = FastAPI(title="Reading Companion Voice Relay", version=__version__, lifespan=lifespan)
admin_page.register_admin_routes(app)


# --------------------------------------------------------------------------
# health / config
# --------------------------------------------------------------------------


@app.get("/healthz")
async def healthz():
    settings = get_settings()
    return {"status": "ok", "service": SERVICE_NAME, "version": settings.relay_version}


@app.get("/readyz")
async def readyz():
    settings = get_settings()
    report = settings.ready_report()
    ready = report["asr"] == "configured" and report["llm"] == "configured"
    return JSONResponse(status_code=200 if ready else 503, content={"status": "ready" if ready else "degraded", **report})


@app.get("/api/v1/device/config")
async def device_config(request: Request):
    settings = get_settings()
    try:
        check_device(
            settings,
            request.headers.get("authorization"),
            request.headers.get("x-device-id"),
        )
    except ProtocolError as exc:
        return JSONResponse(status_code=401 if exc.code == ErrorCode.UNAUTHORIZED else 403, content={"error": exc.code.value})
    return {
        "protocolVersion": protocol.PROTOCOL_VERSION,
        "audio": {
            "sampleRate": protocol.AUDIO_SAMPLE_RATE,
            "channels": protocol.AUDIO_CHANNELS,
            "format": protocol.AUDIO_FORMAT,
            "frameMs": protocol.AUDIO_FRAME_MS,
        },
        "limits": {
            "maxAudioSeconds": settings.max_audio_seconds,
            "maxReplyChars": settings.max_reply_chars,
        },
        "features": {
            "partialAsr": settings.asr_provider != "mock" or True,
            "streamingLlm": True,
            "legacyFallback": True,
            "textToImage": settings.t2i_provider == "mock" or bool(settings.dashscope_api_key),
        },
    }


@app.get("/metrics")
async def prometheus_metrics():
    metrics.observe_rss()
    return Response(generate_latest(), media_type=CONTENT_TYPE_LATEST)


# --------------------------------------------------------------------------
# realtime voice WebSocket
# --------------------------------------------------------------------------


@app.websocket("/api/v1/voice/stream")
async def voice_stream(websocket: WebSocket) -> None:
    settings = get_settings()
    if app.state.ws_connections >= settings.max_ws_connections:
        # Last-resort protection for multi-device deployments: one device in
        # a reconnect loop must not starve the others. 1013 = try again later.
        metrics.ws_rejected_total.labels(reason="connections").inc()
        log.warning(
            "ws connection rejected: at capacity",
            extra={"open": app.state.ws_connections, "limit": settings.max_ws_connections},
        )
        await websocket.accept()
        await websocket.close(code=1013, reason=ErrorCode.SERVER_OVERLOADED.value)
        return
    try:
        device_id = check_device(
            settings,
            websocket.headers.get("authorization"),
            websocket.headers.get("x-device-id"),
        )
    except ProtocolError as exc:
        log.warning("ws auth rejected", extra={"code": exc.code.value, "detail": exc.detail})
        code = WS_CLOSE_UNAUTHORIZED if exc.code == ErrorCode.UNAUTHORIZED else WS_CLOSE_FORBIDDEN
        await websocket.close(code=code, reason=exc.code.value)
        return

    proto_version = websocket.headers.get("x-voice-protocol")
    if proto_version is not None and proto_version.strip() != str(protocol.PROTOCOL_VERSION):
        await websocket.close(code=WS_CLOSE_PROTOCOL, reason=ErrorCode.BAD_PROTOCOL.value)
        return
    if proto_version is None:
        log.warning("ws missing X-Voice-Protocol", extra={"deviceId": device_id})

    await websocket.accept()
    app.state.ws_connections += 1
    metrics.ws_connections_active.inc()
    log.info("ws connected", extra={"deviceId": device_id, "client": websocket.client.host if websocket.client else ""})

    outbound: asyncio.Queue[str] = asyncio.Queue(maxsize=OUTBOUND_QUEUE_MAX)
    writer = asyncio.create_task(_outbound_writer(websocket, outbound))

    session: VoiceSession | None = None
    pre_start_messages = 0

    def send_json(frame: str) -> None:
        try:
            outbound.put_nowait(frame)
        except asyncio.QueueFull:  # device not draining: drop rather than grow
            log.error("outbound queue full", extra={"deviceId": device_id})

    def on_finish(done: VoiceSession) -> None:
        nonlocal session
        if session is done:
            # Keep the object around (state DONE) so a duplicate audio.end or
            # late frame is answered without crashing; next session.start
            # replaces it.
            app.state.sessions.discard(done)

    try:
        while True:
            try:
                message = await asyncio.wait_for(
                    websocket.receive(), timeout=settings.session_idle_timeout_seconds
                )
            except asyncio.TimeoutError:
                if session is None or session.finished:
                    log.info("ws idle timeout", extra={"deviceId": device_id})
                    await websocket.close(code=1000, reason="idle_timeout")
                    break
                continue  # mid-session: keep waiting for upstream events

            if message.get("type") == "websocket.disconnect":
                break

            if (data := message.get("bytes")) is not None:
                pre_start_messages += 1
                if session is None:
                    if pre_start_messages > AUTH_GRACE_MESSAGES:
                        await websocket.close(code=WS_CLOSE_PROTOCOL, reason=ErrorCode.BAD_PROTOCOL.value)
                        break
                    continue  # binary before session.start: ignore a few
                await session.handle_binary(data)
                continue

            text = message.get("text")
            if text is None:
                continue
            try:
                event = parse_device_event(text)
            except ProtocolError as exc:
                log.warning("bad device frame", extra={"deviceId": device_id, "detail": exc.detail})
                code = exc.code
                if session is not None:
                    session._send(  # protocol error is always reported on the wire
                        protocol.error_event(session.request_id, session.session_id, code)
                    )
                else:
                    await websocket.close(code=WS_CLOSE_PROTOCOL, reason=code.value)
                    break
                continue

            # ---- session.start ----
            if event.type == "session.start":
                if session is not None and not session.finished:
                    session._send(
                        protocol.error_event(
                            session.request_id, session.session_id, ErrorCode.BAD_PROTOCOL
                        )
                    )
                    continue
                if not event.audio_supported():
                    send_json(
                        protocol.error_event(
                            event.requestId, protocol.new_id(), ErrorCode.AUDIO_FORMAT_UNSUPPORTED
                        )
                    )
                    continue
                active = [s for s in app.state.sessions if not s.finished]
                if len(active) >= settings.max_concurrent_sessions:
                    metrics.ws_rejected_total.labels(reason="sessions").inc()
                    log.warning(
                        "session rejected: global capacity",
                        extra={"deviceId": device_id, "active": len(active)},
                    )
                    send_json(
                        protocol.error_event(
                            event.requestId, protocol.new_id(), ErrorCode.SERVER_OVERLOADED
                        )
                    )
                    continue
                if sum(1 for s in active if s.device_id == device_id) >= settings.max_sessions_per_device:
                    metrics.ws_rejected_total.labels(reason="device_sessions").inc()
                    log.warning("session rejected: device capacity", extra={"deviceId": device_id})
                    send_json(
                        protocol.error_event(
                            event.requestId, protocol.new_id(), ErrorCode.SERVER_OVERLOADED
                        )
                    )
                    continue
                # The authenticated header identity wins over the message field
                # so per-device limits and logs cannot be dodged by a device
                # that sends a different deviceId per session.start.
                if device_id:
                    event.deviceId = device_id
                session = VoiceSession(settings, send_json, event, on_finish)
                app.state.sessions.add(session)
                await session.handle_session_start()
                continue

            if session is None:
                # Non-session.start before any session: reject the connection.
                await websocket.close(code=WS_CLOSE_PROTOCOL, reason=ErrorCode.BAD_PROTOCOL.value)
                break

            # ---- session-scoped events ----
            if getattr(event, "requestId", None) != session.request_id or (
                getattr(event, "sessionId", "") and event.sessionId != session.session_id
            ):
                session._send(
                    protocol.error_event(session.request_id, session.session_id, ErrorCode.BAD_PROTOCOL)
                )
                continue

            try:
                if event.type == "audio.start":
                    if session.finished:
                        session._send(
                            protocol.error_event(session.request_id, session.session_id, ErrorCode.BAD_PROTOCOL)
                        )
                        continue
                    await session.handle_audio_start(event)
                elif event.type == "audio.end":
                    if session.finished:
                        session._send(
                            protocol.error_event(session.request_id, session.session_id, ErrorCode.BAD_PROTOCOL)
                        )
                        continue
                    await session.handle_audio_end(event)
                elif event.type == "session.cancel":
                    await session.handle_cancel(event)
            except ProtocolError as exc:
                # Reported on the wire; the connection itself stays usable.
                session._send(protocol.error_event(session.request_id, session.session_id, exc.code))
    except WebSocketDisconnect:
        log.info("ws disconnected", extra={"deviceId": device_id})
    finally:
        if session is not None and not session.finished:
            await session.abort("device_disconnect")
        if session is not None:
            app.state.sessions.discard(session)
        writer.cancel()
        try:
            await writer
        except (asyncio.CancelledError, Exception):
            pass
        app.state.ws_connections -= 1
        metrics.ws_connections_active.dec()


async def _outbound_writer(websocket: WebSocket, queue: asyncio.Queue[str]) -> None:
    while True:
        frame = await queue.get()
        await websocket.send_text(frame)


# --------------------------------------------------------------------------
# text endpoint (admin token or relay device token; disabled when neither is
# configured). Used by the device for Ask-AI over relay.
# --------------------------------------------------------------------------


def _text_endpoint_guard(settings: Settings, request: Request) -> JSONResponse | None:
    """Shared auth for /chat and /image: admin OR device token. None = allowed."""
    if not settings.relay_admin_token and not settings.relay_device_token:
        return JSONResponse(status_code=404, content={"error": "disabled"})
    authorization = request.headers.get("authorization")
    try:
        check_admin(settings, authorization)
        return None
    except ProtocolError:
        pass
    try:
        check_device(settings, authorization, request.headers.get("x-device-id"))
        return None
    except ProtocolError:
        return JSONResponse(status_code=401, content={"error": ErrorCode.UNAUTHORIZED.value})


class ChatRequest(BaseModel):
    requestId: str = ""
    deviceId: str = "debug-client"
    text: str
    stream: bool = True
    maxTokens: int | None = None
    mode: str = "ask"


@app.post("/api/v1/chat")
async def chat(request: Request, body: ChatRequest):
    settings = get_settings()
    guard = _text_endpoint_guard(settings, request)
    if guard is not None:
        return guard

    request_id = body.requestId or protocol.new_id()

    # Dictionary cards come from the offline data by default: the facts
    # (readings, radical, senses, 组词) must not be model output. DICT_SOURCE=llm
    # restores the generated cards; when the data is missing we degrade to the
    # LLM path rather than failing the lookup entirely.
    if body.mode == "dict" and settings.dict_source != "llm":
        source = dict_source.get_dict_source(settings.dict_data_dir)
        if source is not None:
            card = source.card(body.text)
            strokes = source.stroke_payload(body.text)
            log.info(
                "dict card served",
                extra={
                    "source": "local",
                    "bytes": len(card.encode("utf-8")),
                    "strokes": len(strokes or ""),
                },
            )
            if not body.stream:
                # "strokes" goes before "usage": the device reads the body
                # into a 4 KB window and a truncated tail must never cost the
                # card facts (usage is informational for this path).
                response = {"requestId": request_id, "text": card}
                if strokes:
                    response["strokes"] = strokes
                response["usage"] = {"inputTokens": 0, "outputTokens": 0}
                return response

            async def local_card_events():
                yield _sse({"type": "llm.start", "requestId": request_id})
                yield _sse({"type": "llm.delta", "requestId": request_id, "text": card})
                yield _sse({"type": "session.done", "requestId": request_id})

            return StreamingResponse(local_card_events(), media_type="text/event-stream")
        log.warning("dict data unavailable; serving this card from the LLM")

    dict_mode = body.mode == "dict"
    system = settings.llm_dict_system_prompt if dict_mode else None
    max_tokens = body.maxTokens or (settings.dict_max_tokens if dict_mode else settings.max_llm_output_tokens)
    provider = make_llm_provider(settings)

    if not body.stream:
        try:
            text, usage = await provider.stream(body.text, [], max_tokens, _noop, system=system)
            metrics.llm_requests_total.labels(result="ok").inc()
            return {
                "requestId": request_id,
                "text": text,
                "usage": {
                    "inputTokens": usage.input_tokens,
                    "outputTokens": usage.output_tokens,
                },
            }
        except ProviderError as exc:
            metrics.llm_requests_total.labels(result="error").inc()
            return JSONResponse(status_code=502, content={"code": exc.code.value})

    async def events():
        try:
            yield _sse({"type": "llm.start", "requestId": request_id})
            async for piece in _iter_deltas(provider, body.text, max_tokens, system):
                yield _sse({"type": "llm.delta", "requestId": request_id, "text": piece})
            yield _sse({"type": "session.done", "requestId": request_id})
        except ProviderError as exc:
            yield _sse({"type": "error", "requestId": request_id, "code": exc.code.value})
        finally:
            close = getattr(provider, "close", None)
            if close is not None:
                try:
                    await close()
                except Exception:
                    pass

    return StreamingResponse(events(), media_type="text/event-stream")


# --------------------------------------------------------------------------
# text-to-image endpoint (same auth as /chat). The generation call returns
# metadata + a waiting phrase; the device then fetches the BMP via the asset
# URL. Mock provider by default (see relay/t2i.py).
# --------------------------------------------------------------------------


class ImageRequest(BaseModel):
    requestId: str = ""
    text: str = ""
    # Optional generation parameters. style is a DashScope V1 enum value
    # (relay/t2i.py DASHSCOPE_STYLES); size is "W*H". Both are validated in
    # t2i.render and unknown values fail with T2I_BAD_PARAM (HTTP 400).
    style: str = ""
    size: str = ""
    # Optional edit source for the device's 修改 flow: {"data": <base64 BMP>,
    # "format": "bmp", ...}. Present = image-to-image instead of pure t2i.
    image: dict | None = None


@app.post("/api/v1/image")
async def generate_image(request: Request, body: ImageRequest):
    settings = get_settings()
    guard = _text_endpoint_guard(settings, request)
    if guard is not None:
        return guard

    request_id = body.requestId or protocol.new_id()
    started = time.monotonic()
    try:
        bmp = await t2i.render(settings, body.text, style=body.style, size=body.size, image=body.image)
    except t2i.T2iError as exc:
        metrics.t2i_requests_total.labels(result="error").inc()
        log.warning("t2i failed", extra={"code": exc.code, "detail": exc.detail})
        status = 400 if exc.code in ("T2I_EMPTY_PROMPT", "T2I_BAD_PARAM") else 502
        return JSONResponse(status_code=status, content={"requestId": request_id, "code": exc.code})

    asset_id = t2i.save_asset(settings, bmp)
    elapsed = time.monotonic() - started
    metrics.t2i_requests_total.labels(result="ok").inc()
    log.info(
        "t2i image served",
        extra={
            "provider": settings.t2i_provider,
            "bytes": len(bmp),
            "seconds": round(elapsed, 2),
        },
    )
    return {
        "requestId": request_id,
        "text": t2i.pick_phrase(),
        "image": {
            "id": asset_id,
            "url": f"/api/v1/image/{asset_id}",
            "width": settings.t2i_width,
            "height": settings.t2i_height,
            "bytes": len(bmp),
            "format": "bmp",
        },
        "usage": {"seconds": round(elapsed, 1)},
    }


@app.get("/api/v1/image/{asset_id}")
async def image_asset(asset_id: str, request: Request):
    settings = get_settings()
    guard = _text_endpoint_guard(settings, request)
    if guard is not None:
        return guard

    data = t2i.load_asset(settings, asset_id)
    if data is None:
        return JSONResponse(status_code=404, content={"error": "not_found"})
    return Response(
        content=data,
        media_type="image/bmp",
        headers={"Cache-Control": "private, max-age=3600"},
    )


async def _noop(_piece: str) -> None:
    return None


async def _iter_deltas(provider, text: str, max_tokens: int, system: str | None = None):
    """Bridge the callback-based provider into an async iterator."""
    queue: asyncio.Queue[str | None] = asyncio.Queue()

    async def on_delta(piece: str) -> None:
        await queue.put(piece)

    task = asyncio.create_task(provider.stream(text, [], max_tokens, on_delta, system=system))

    async def pump():
        try:
            await task
        finally:
            await queue.put(None)

    pump_task = asyncio.create_task(pump())
    try:
        while True:
            piece = await queue.get()
            if piece is None:
                break
            yield piece
        await pump_task
    finally:
        for t in (task, pump_task):
            if not t.done():
                t.cancel()


def _sse(obj: dict) -> str:
    return f"data: {orjson.dumps(obj).decode()}\n\n"

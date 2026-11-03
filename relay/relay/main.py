"""Voice Relay HTTP/WebSocket application.

Routes (docs section 17.5):
  GET  /healthz                 liveness probe, no auth, no upstream checks
  GET  /readyz                  config readiness, no auth
  GET  /admin                   setup page, host-only (loopback) + admin token
  GET  /admin/status            JSON backing the setup page
  GET  /api/v1/device/config    device capabilities, device token
  WSS  /api/v1/voice/stream     the device's realtime channel
  POST /api/v1/chat             admin-only text debug (SSE or JSON)
  GET  /api/v1/learning/words   device token: word book listing
  POST /api/v1/learning/words   device token: word book batch add
  POST /api/v1/learning/review  device token: review event (idempotent)
  POST /api/v1/pomodoro/sessions device token: completed focus sessions (idempotent)
  GET  /api/v1/pomodoro/stats   device token: daily focus aggregates
  GET  /metrics                 prometheus, internal exposure only
"""

from __future__ import annotations

import asyncio
import logging
import time
from collections import deque
from contextlib import asynccontextmanager

import orjson
from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse, Response, StreamingResponse
from prometheus_client import CONTENT_TYPE_LATEST, generate_latest
from pydantic import BaseModel, Field

from . import SERVICE_NAME, __version__
from . import admin_page, dict_source, learning_store, metrics, pigeon_store, pomodoro_store, protocol, t2i
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
    # Pomodoro focus sessions: same one-connection-per-process pattern.
    pomodoro = pomodoro_store.PomodoroStore(settings.pomodoro_db_path)
    pomodoro.open()
    app.state.pomodoro = pomodoro
    log.info("pomodoro store ready", extra={"db": str(pomodoro.db_path)})
    # Pigeon: durable store-and-forward plus ephemeral presence. Presence is
    # intentionally in-process (single worker); a restart just clears the
    # online list until the next heartbeat.
    pigeon = pigeon_store.PigeonStore(
        settings.pigeon_db_path,
        ttl_days=settings.pigeon_message_ttl_days,
        max_per_recipient=settings.pigeon_max_messages_per_recipient,
    )
    pigeon.open()
    app.state.pigeon = pigeon
    app.state.pigeon_presence = pigeon_store.PigeonPresence(settings.pigeon_presence_ttl_seconds)
    app.state.pigeon_rate = {}
    log.info("pigeon store ready", extra={"db": str(pigeon.db_path)})
    yield
    # Graceful shutdown: stop accepting, cancel upstream work in bounded time.
    sessions: set[VoiceSession] = app.state.sessions
    if sessions:
        log.info("cancelling sessions at shutdown", extra={"count": len(sessions)})
        await asyncio.gather(*(s.abort("server_shutdown") for s in list(sessions)), return_exceptions=True)
    pigeon.close()
    pomodoro.close()
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
            "partialAsr": settings.asr_provider == "mock" or True,
            "streamingLlm": True,
            "legacyFallback": True,
            "textToImage": settings.t2i_provider == "mock" or bool(settings.dashscope_api_key),
            # Vocabulary learning: the store is opened in the lifespan (boot
            # fails without it), so reaching this endpoint means learning is
            # available. Firmware gates its learning menu on this key.
            "learning": True,
            # Pomodoro focus timer: same lifespan-store reasoning as learning.
            # Firmware gates its focus-timer menu on this key.
            "pomodoro": True,
            # Pigeon device-to-device messages (store-and-forward + presence).
            "pigeon": True,
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
    dict_mode = body.mode == "dict"
    word_mode = body.mode == "word"
    if dict_mode and settings.dict_source != "llm":
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

    system = None
    if dict_mode:
        system = settings.llm_dict_system_prompt
    elif word_mode:
        system = settings.llm_word_system_prompt
    card_mode = dict_mode or word_mode
    max_tokens = body.maxTokens or (settings.dict_max_tokens if card_mode else settings.max_llm_output_tokens)
    provider = make_llm_provider(settings)

    if not body.stream:
        try:
            text, usage = await provider.stream(body.text, [], max_tokens, _noop, system=system)
            metrics.llm_requests_total.labels(result="ok").inc()
            if card_mode:
                # The word-card path used to leave only a bare httpx line:
                # which word was served (and how long it took to generate)
                # was invisible in the journal.
                log.info(
                    "llm card served",
                    extra={"mode": body.mode, "word": body.text[:32], "chars": len(text), "outputTokens": usage.output_tokens},
                )
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
            if card_mode:
                log.warning(
                    "llm card failed",
                    extra={"mode": body.mode, "word": body.text[:32], "code": exc.code.value},
                )
            return JSONResponse(status_code=502, content={"code": exc.code.value})

    async def events():
        chars = 0
        try:
            yield _sse({"type": "llm.start", "requestId": request_id})
            async for piece in _iter_deltas(provider, body.text, max_tokens, system):
                chars += len(piece)
                yield _sse({"type": "llm.delta", "requestId": request_id, "text": piece})
            if card_mode:
                log.info(
                    "llm card served",
                    extra={"mode": body.mode, "word": body.text[:32], "chars": chars},
                )
            yield _sse({"type": "session.done", "requestId": request_id})
        except ProviderError as exc:
            if card_mode:
                log.warning(
                    "llm card failed",
                    extra={"mode": body.mode, "word": body.text[:32], "code": exc.code.value},
                )
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


# --------------------------------------------------------------------------
# vocabulary learning (device token; word book + review event sync)
# --------------------------------------------------------------------------

LEARNING_MAX_WORD_BATCH = 200


def _data_device(settings: Settings, request: Request, feature: str) -> tuple[str | None, JSONResponse | None]:
    """Auth for device data endpoints (/learning/*, /pomodoro/*): device token
    only (the device is the sole client of each).

    Returns (device_id, None) on success or (None, error_response).
    """
    try:
        device_id = check_device(settings, request.headers.get("authorization"), request.headers.get("x-device-id"))
        return device_id, None
    except ProtocolError as exc:
        # nginx keeps the status code, but only this line tells the operator
        # why a device call was rejected (bad token vs wrong device id).
        log.warning(f"{feature} auth rejected", extra={"code": exc.code.value, "detail": exc.detail})
        return None, JSONResponse(
            status_code=401 if exc.code == ErrorCode.UNAUTHORIZED else 403,
            content={"error": exc.code.value},
        )


def _learning_device(settings: Settings, request: Request) -> tuple[str | None, JSONResponse | None]:
    """Auth for /learning/* (log tag "learning auth rejected")."""
    return _data_device(settings, request, "learning")


class LearningWordsRequest(BaseModel):
    words: list[str]
    source: str = "lookup"


class LearningReviewRequest(BaseModel):
    word: str
    rating: int
    clientEventId: str
    occurredAt: str = ""


@app.get("/api/v1/learning/words")
async def learning_words(request: Request, state: str = "", limit: int = 500):
    settings = get_settings()
    device_id, error = _learning_device(settings, request)
    if error is not None:
        return error
    # "" and "all" mean no filter; anything outside the two real states is a
    # client bug — answering count:0 would read as "the word book is empty".
    if state and state not in ("all", "active", "removed"):
        return JSONResponse(
            status_code=400,
            content={"error": "invalid_state", "allowed": ["active", "removed"]},
        )
    rows = app.state.learning.list_words(device_id, None if state in ("", "all") else state, max(1, min(limit, 500)))
    log.info("learning word list", extra={"deviceId": device_id, "state": state or "all", "count": len(rows)})
    return {
        "count": len(rows),
        "words": [
            {"word": r["word"], "addedAt": r["added_at"], "state": r["state"], "source": r["source"]} for r in rows
        ],
    }


@app.post("/api/v1/learning/words")
async def learning_add_words(request: Request, body: LearningWordsRequest):
    settings = get_settings()
    device_id, error = _learning_device(settings, request)
    if error is not None:
        return error
    if not body.words or len(body.words) > LEARNING_MAX_WORD_BATCH:
        return JSONResponse(
            status_code=400,
            content={"error": "invalid_words", "max": LEARNING_MAX_WORD_BATCH},
        )
    added = app.state.learning.add_words(device_id, [(w, body.source) for w in body.words])
    log.info("learning word sync", extra={"deviceId": device_id, "requested": len(body.words), "added": added})
    return {"ok": True, "accepted": len(body.words), "added": added}


@app.post("/api/v1/learning/words/remove")
async def learning_remove_words(request: Request, body: LearningWordsRequest):
    settings = get_settings()
    device_id, error = _learning_device(settings, request)
    if error is not None:
        return error
    if not body.words or len(body.words) > LEARNING_MAX_WORD_BATCH:
        return JSONResponse(
            status_code=400,
            content={"error": "invalid_words", "max": LEARNING_MAX_WORD_BATCH},
        )
    removed = app.state.learning.remove_words(device_id, body.words)
    log.info("learning word removal", extra={"deviceId": device_id, "requested": len(body.words), "removed": removed})
    return {"ok": True, "accepted": len(body.words), "removed": removed}


@app.get("/api/v1/learning/review")
async def learning_review_queue(request: Request, limit: int = 20):
    settings = get_settings()
    device_id, error = _learning_device(settings, request)
    if error is not None:
        return error
    rows, due = app.state.learning.review_queue(device_id, limit=max(1, min(limit, 50)))
    log.info("learning review queue", extra={"deviceId": device_id, "served": len(rows), "due": due})
    return {"count": len(rows), "due": due, "words": rows}


@app.post("/api/v1/learning/review")
async def learning_review(request: Request, body: LearningReviewRequest):
    settings = get_settings()
    device_id, error = _learning_device(settings, request)
    if error is not None:
        return error
    word = body.word.strip()
    if not word or not body.clientEventId or not 1 <= body.rating <= 4:
        return JSONResponse(status_code=400, content={"error": "invalid_review"})
    extras = {"occurredAt": body.occurredAt} if body.occurredAt else None
    stored = app.state.learning.record_review(device_id, word, body.rating, body.clientEventId, extras)
    log.info(
        "learning review event",
        extra={"deviceId": device_id, "word": word, "rating": body.rating, "duplicate": not stored},
    )
    return {"ok": True, "duplicate": not stored}


# --------------------------------------------------------------------------
# pomodoro focus sessions (device token; session upload + daily stats)
# --------------------------------------------------------------------------

POMODORO_MAX_SESSION_BATCH = 50


class PomodoroSessionItem(BaseModel):
    clientEventId: str
    kind: str  # work | break | long_break (validated at the edge)
    startedAt: str  # device-local ISO8601 as written by the device clock
    endedAt: str
    minutes: int
    completed: bool = True  # False = user ended early (partial session)


class PomodoroSessionsRequest(BaseModel):
    sessions: list[PomodoroSessionItem]


def _pomodoro_device(settings: Settings, request: Request) -> tuple[str | None, JSONResponse | None]:
    """Auth for /pomodoro/* (log tag "pomodoro auth rejected")."""
    return _data_device(settings, request, "pomodoro")


@app.post("/api/v1/pomodoro/sessions")
async def pomodoro_sessions(request: Request, body: PomodoroSessionsRequest):
    settings = get_settings()
    device_id, error = _pomodoro_device(settings, request)
    if error is not None:
        return error
    sessions = body.sessions
    if not sessions or len(sessions) > POMODORO_MAX_SESSION_BATCH:
        return JSONResponse(
            status_code=400,
            content={"error": "invalid_sessions", "max": POMODORO_MAX_SESSION_BATCH},
        )
    entries: list[dict] = []
    for s in sessions:
        if (
            not s.clientEventId
            or s.kind not in pomodoro_store.SESSION_KINDS
            or not s.startedAt
            or not s.endedAt
            or not pomodoro_store.MIN_MINUTES <= s.minutes <= pomodoro_store.MAX_MINUTES
        ):
            return JSONResponse(
                status_code=400,
                content={"error": "invalid_session", "allowedKinds": list(pomodoro_store.SESSION_KINDS)},
            )
        entries.append(
            {
                "client_event_id": s.clientEventId,
                "kind": s.kind,
                "started_at": s.startedAt,
                "ended_at": s.endedAt,
                "minutes": s.minutes,
                "completed": 1 if s.completed else 0,
            }
        )
    stored = app.state.pomodoro.record_sessions(device_id, entries)
    log.info(
        "pomodoro session sync",
        extra={"deviceId": device_id, "requested": len(entries), "stored": stored},
    )
    return {"ok": True, "accepted": len(entries), "stored": stored}


@app.get("/api/v1/pomodoro/stats")
async def pomodoro_stats(request: Request, days: int = 30):
    settings = get_settings()
    device_id, error = _pomodoro_device(settings, request)
    if error is not None:
        return error
    days = max(1, min(days, 366))
    stats = app.state.pomodoro.stats(device_id, days)
    log.info("pomodoro stats", extra={"deviceId": device_id, "days": days, "rows": len(stats["days"])})
    return stats


# --------------------------------------------------------------------------
# pigeon device-to-device messages (device token; heartbeat + inbox + send)
# --------------------------------------------------------------------------

PIGEON_SEND_WINDOW_SECONDS = 60.0
PIGEON_MAX_CLIENT_EVENT_ID = 80


class PigeonHeartbeatRequest(BaseModel):
    name: str = ""


class PigeonSendRequest(BaseModel):
    to: str
    text: str
    clientEventId: str = ""


class PigeonAckRequest(BaseModel):
    ids: list[int] = Field(default_factory=list)
    upToId: int | None = None


def _pigeon_device(settings: Settings, request: Request) -> tuple[str | None, JSONResponse | None]:
    """Auth for /pigeon/* (log tag "pigeon auth rejected")."""
    return _data_device(settings, request, "pigeon")


def _pigeon_known_devices(settings: Settings) -> list[str]:
    """Ids the relay can address: pinned ids plus per-device token entries."""
    return sorted(set(settings.allowed_devices) | set(settings.device_token_map()))


def _pigeon_devices(settings: Settings, self_id: str) -> list[dict]:
    """All addressable peers, online or not.

    The durable device registry (written by every heartbeat) is what makes an
    offline device selectable. A configured deployment also lists pinned ids
    that have never checked in, so an admin can pre-provision a reader.
    """
    presence = app.state.pigeon_presence
    registered = {row["id"]: row for row in app.state.pigeon.devices(exclude_id=self_id)}
    known = _pigeon_known_devices(settings)
    ids = set(known) if known else set(registered)
    ids.discard(self_id)
    devices = [
        {
            "id": device_id,
            "name": presence.name(device_id) or registered.get(device_id, {}).get("name", "") or device_id,
            "online": presence.is_online(device_id),
        }
        for device_id in ids
    ]
    # Online first, then by display name/id, so the small firmware list cap
    # never hides a live peer behind a pile of stale registrations.
    devices.sort(key=lambda d: (not d["online"], d["name"].lower(), d["id"]))
    return devices


def _pigeon_rate_ok(device_id: str, limit: int) -> bool:
    now = time.monotonic()
    buckets: dict[str, deque[float]] = app.state.pigeon_rate
    bucket = buckets.setdefault(device_id, deque())
    while bucket and now - bucket[0] >= PIGEON_SEND_WINDOW_SECONDS:
        bucket.popleft()
    if len(bucket) >= max(1, limit):
        return False
    bucket.append(now)
    return True


@app.post("/api/v1/pigeon/heartbeat")
async def pigeon_heartbeat(request: Request, body: PigeonHeartbeatRequest | None = None):
    settings = get_settings()
    device_id, error = _pigeon_device(settings, request)
    if error is not None:
        return error
    name = (body.name if body else "") or request.headers.get("x-device-name", "")
    app.state.pigeon.touch_device(device_id, name)
    app.state.pigeon_presence.touch(device_id, name)
    log.info("pigeon heartbeat", extra={"deviceId": device_id, "deviceName": name[:32]})
    return {
        "ok": True,
        "self": {"id": device_id, "name": app.state.pigeon_presence.name(device_id) or device_id},
        "devices": _pigeon_devices(settings, device_id),
        "unread": app.state.pigeon.unread_count(device_id),
    }


@app.post("/api/v1/pigeon/send")
async def pigeon_send(request: Request, body: PigeonSendRequest):
    settings = get_settings()
    device_id, error = _pigeon_device(settings, request)
    if error is not None:
        return error
    text = body.text.strip()
    if not text:
        return JSONResponse(status_code=400, content={"error": "empty_text"})
    if len(text.encode("utf-8")) > settings.pigeon_max_text_bytes:
        return JSONResponse(
            status_code=400,
            content={"error": "text_too_long", "maxBytes": settings.pigeon_max_text_bytes},
        )
    if len(body.clientEventId) > PIGEON_MAX_CLIENT_EVENT_ID:
        return JSONResponse(status_code=400, content={"error": "client_event_id_too_long"})
    recipient = body.to.strip()
    if not recipient or recipient == device_id:
        return JSONResponse(status_code=400, content={"error": "invalid_recipient"})
    known = _pigeon_known_devices(settings)
    allowed = recipient in known if known else app.state.pigeon.has_device(recipient)
    if not allowed:
        return JSONResponse(status_code=404, content={"error": "unknown_recipient"})
    if not _pigeon_rate_ok(device_id, settings.pigeon_rate_limit_per_minute):
        log.warning("pigeon send rate limited", extra={"deviceId": device_id})
        return JSONResponse(status_code=429, content={"error": "rate_limited"})

    message = app.state.pigeon.send(device_id, recipient, text, body.clientEventId)
    log.info(
        "pigeon send",
        extra={
            "deviceId": device_id,
            "to": recipient,
            "messageId": message["id"],
            "duplicate": message["duplicate"],
            "bytes": len(text.encode("utf-8")),
        },
    )
    return {"ok": True, "message": message, "duplicate": message["duplicate"]}


@app.get("/api/v1/pigeon/inbox")
async def pigeon_inbox(request: Request, after: int = 0, limit: int = 50):
    settings = get_settings()
    device_id, error = _pigeon_device(settings, request)
    if error is not None:
        return error
    after = max(0, after)
    messages = app.state.pigeon.inbox(device_id, after_id=after, limit=limit)
    latest = messages[-1]["id"] if messages else after
    return {
        "messages": messages,
        "unread": app.state.pigeon.unread_count(device_id),
        "latestId": latest,
    }


@app.post("/api/v1/pigeon/ack")
async def pigeon_ack(request: Request, body: PigeonAckRequest):
    settings = get_settings()
    device_id, error = _pigeon_device(settings, request)
    if error is not None:
        return error
    if not body.ids and body.upToId is None:
        return JSONResponse(status_code=400, content={"error": "missing_ids"})
    acked = app.state.pigeon.ack(device_id, ids=body.ids, up_to_id=body.upToId)
    return {"ok": True, "acked": acked}


@app.get("/api/v1/pigeon/history")
async def pigeon_history(request: Request, peer: str = "", limit: int = 50):
    settings = get_settings()
    device_id, error = _pigeon_device(settings, request)
    if error is not None:
        return error
    peer = peer.strip()
    if not peer:
        return JSONResponse(status_code=400, content={"error": "missing_peer"})
    known = _pigeon_known_devices(settings)
    allowed = peer in known if known else app.state.pigeon.has_device(peer)
    if not allowed:
        return JSONResponse(status_code=404, content={"error": "unknown_peer"})
    return {
        "peer": peer,
        "messages": app.state.pigeon.history(device_id, peer, limit=limit),
    }


@app.post("/api/v1/pigeon/clear")
async def pigeon_clear(request: Request, peer: str = ""):
    settings = get_settings()
    device_id, error = _pigeon_device(settings, request)
    if error is not None:
        return error
    peer = peer.strip()
    if not peer:
        return JSONResponse(status_code=400, content={"error": "missing_peer"})
    known = _pigeon_known_devices(settings)
    allowed = peer in known if known else app.state.pigeon.has_device(peer)
    if not allowed:
        return JSONResponse(status_code=404, content={"error": "unknown_peer"})
    deleted = app.state.pigeon.clear_conversation(device_id, peer)
    return {"ok": True, "deleted": deleted}


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

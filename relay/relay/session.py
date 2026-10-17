"""Voice session state machine and orchestration.

One WebSocket carries one active session at a time (docs section 17.6).
Transitions are explicit; raw background threads are not used — every upstream
call runs in an asyncio task owned by the session, and every one of them is
cancelled on device disconnect, cancel or timeout.

  CONNECTED -> SESSION_STARTED -> RECEIVING_AUDIO -> ASR_RUNNING
            -> FINISHING_ASR -> LLM_RUNNING -> DONE
  and from any state: CANCELLED / FAILED / TIMEOUT
"""

from __future__ import annotations

import asyncio
import logging
import time
from collections import deque
from enum import Enum
from typing import Awaitable, Callable

from . import metrics, protocol
from .config import Settings
from .protocol import (
    AudioParams,
    AudioStart,
    AudioEnd,
    ErrorCode,
    ProtocolError,
    SessionCancel,
    SessionStart,
)
from .providers import make_asr_provider, make_llm_provider
from .providers.base import AsrProvider, LlmProvider, ProviderError
from .providers.openclaw import answer_via_openclaw, should_route_to_openclaw

log = logging.getLogger("relay.session")

# Bounded frame ring while the ASR upstream is still connecting (5 s timeout
# budget at 40 ms/frame = 125 frames; 256 KB is ~4 s of audio).
ASR_PREBUFFER_BYTES = 256 * 1024
MAX_HISTORY_TURNS = 4
# The ASR provider emits one sentence_end final per sentence, so a single
# capture can produce several asr.final events. They are joined into one
# transcript (bounding memory on a stuck upstream).
MAX_TRANSCRIPT_CHARS = 2000


def _join_transcript(prev: str, text: str) -> str:
    """Join sentence finals; a space only between adjacent ASCII words."""
    if not prev:
        return text
    if prev[-1].isascii() and prev[-1].isalnum() and text[0].isascii() and text[0].isalnum():
        return f"{prev} {text}"
    return prev + text


class SessionState(str, Enum):
    CONNECTED = "CONNECTED"
    SESSION_STARTED = "SESSION_STARTED"
    RECEIVING_AUDIO = "RECEIVING_AUDIO"
    ASR_RUNNING = "ASR_RUNNING"
    FINISHING_ASR = "FINISHING_ASR"
    LLM_RUNNING = "LLM_RUNNING"
    DONE = "DONE"
    CANCELLED = "CANCELLED"
    FAILED = "FAILED"
    TIMEOUT = "TIMEOUT"


TERMINAL_STATES = {SessionState.DONE, SessionState.CANCELLED, SessionState.FAILED, SessionState.TIMEOUT}

_TRANSITIONS: dict[SessionState, set[SessionState]] = {
    SessionState.CONNECTED: {SessionState.SESSION_STARTED, SessionState.CANCELLED, SessionState.FAILED},
    SessionState.SESSION_STARTED: {
        SessionState.RECEIVING_AUDIO,
        SessionState.CANCELLED,
        SessionState.FAILED,
        SessionState.TIMEOUT,
    },
    SessionState.RECEIVING_AUDIO: {
        SessionState.ASR_RUNNING,
        SessionState.FINISHING_ASR,
        SessionState.CANCELLED,
        SessionState.FAILED,
        SessionState.TIMEOUT,
    },
    SessionState.ASR_RUNNING: {
        SessionState.FINISHING_ASR,
        SessionState.CANCELLED,
        SessionState.FAILED,
        SessionState.TIMEOUT,
    },
    SessionState.FINISHING_ASR: {
        SessionState.LLM_RUNNING,
        SessionState.CANCELLED,
        SessionState.FAILED,
        SessionState.TIMEOUT,
    },
    SessionState.LLM_RUNNING: {
        SessionState.DONE,
        SessionState.CANCELLED,
        SessionState.FAILED,
        SessionState.TIMEOUT,
    },
}


class LatencyMarks:
    """t0..t9 per docs section 17.14, stored as perf_counter timestamps."""

    def __init__(self) -> None:
        self.marks: dict[str, float] = {"t0": time.perf_counter()}

    def mark(self, name: str) -> None:
        self.marks.setdefault(name, time.perf_counter())

    def ms(self, a: str, b: str) -> int | None:
        if a in self.marks and b in self.marks:
            return round((self.marks[a] - self.marks[b]) * 1000)
        return None

    def report(self) -> dict[str, int | None]:
        asr_first = self.ms("t4", "t2")
        asr_final = self.ms("t5", "t3")
        if asr_final is not None and asr_final < 0:
            # Accumulation mode: DashScope's sentence detector emits finals
            # mid-stream, before the device's own VAD/audio.end, so once the
            # audio ends there is nothing left to wait for.
            asr_final = 0
        # Docs §17.14: LLM first-token latency runs from the LLM request (t6).
        # Fall back to the ASR final for OpenClaw turns, which send their
        # request without marking t6.
        llm_first = self.ms("t7", "t6")
        if llm_first is None:
            llm_first = self.ms("t7", "t5")
        llm_final = self.ms("t8", "t6")
        if llm_final is None:
            llm_final = self.ms("t8", "t5")
        return {
            "connect": self.ms("t1", "t0"),
            "asrFirstPartial": asr_first,
            "asrFinal": asr_final,
            "llmFirstToken": llm_first,
            "llmFinal": llm_final,
            "visibleTotal": self.ms("t9", "t3"),
        }


class VoiceSession:
    """Owns one requestId's pipeline. All I/O to the device is via send_json."""

    def __init__(
        self,
        settings: Settings,
        send_json: Callable[[str], None],
        start: SessionStart,
        on_finish: Callable[["VoiceSession"], None],
    ) -> None:
        self.settings = settings
        self._send = send_json
        self._on_finish = on_finish
        self.request_id = start.requestId
        self.device_id = start.deviceId
        self.mode = start.mode
        self.audio_params: AudioParams = start.audio
        self.session_id = protocol.new_id()
        self.state = SessionState.CONNECTED
        self.marks = LatencyMarks()
        self.seq = 0

        self.frames_received = 0
        self.bytes_received = 0
        self.frames_dropped = 0
        self.audio_duration_ms = 0

        self._asr: AsrProvider | None = None
        self._asr_start_task: asyncio.Task | None = None
        self._asr_ready = asyncio.Event()
        self._llm_task: asyncio.Task | None = None
        self._prebuffer: deque[bytes] = deque()
        self._prebuffer_bytes = 0
        self._audio_ended = False
        self._transcript: str | None = None
        self._history: list[dict[str, str]] = []
        self._result = "failed"
        self._error_sent = False
        self._finished = False

    # ------------------------------------------------------------ transitions
    def _transition(self, new: SessionState) -> bool:
        if new in TERMINAL_STATES:
            self.state = new
            return True
        allowed = _TRANSITIONS.get(self.state, set())
        if new not in allowed:
            log.error(
                "illegal transition",
                extra={"requestId": self.request_id, "from": self.state.value, "to": new.value},
            )
            return False
        self.state = new
        return True

    # ---------------------------------------------------------------- inbound
    async def handle_session_start(self) -> None:
        self._transition(SessionState.SESSION_STARTED)
        metrics.voice_sessions_active.inc()
        self._send(
            protocol.session_started(self.request_id, self.session_id),
        )
        log.info(
            "session started",
            extra={
                "requestId": self.request_id,
                "sessionId": self.session_id,
                "deviceId": self.device_id,
                "audio": self.audio_params.model_dump(),
            },
        )

    async def handle_audio_start(self, msg: AudioStart) -> None:
        self.marks.mark("t1")
        self._transition(SessionState.RECEIVING_AUDIO)
        # Start the ASR upstream now so result-generated can arrive while the
        # user is still speaking; frames buffer until task-started.
        self._asr_start_task = asyncio.create_task(self._start_asr())

    async def handle_binary(self, data: bytes) -> None:
        if self.state in TERMINAL_STATES:
            self.frames_dropped += 1
            metrics.audio_frames_dropped_total.inc()
            return
        if self._audio_ended:
            self.frames_dropped += 1
            metrics.audio_frames_dropped_total.inc()
            return
        if not data:
            return
        self.frames_received += 1
        self.bytes_received += len(data)
        self.audio_duration_ms += len(data) // 32  # 16 samples/ms * 2 bytes
        metrics.audio_frames_received_total.inc()
        self.marks.mark("t2")
        if self.state == SessionState.RECEIVING_AUDIO and self._asr_ready.is_set():
            self._transition(SessionState.ASR_RUNNING)

        if self.audio_duration_ms > self.settings.max_audio_seconds * 1000:
            await self._fail(ErrorCode.AUDIO_TIMEOUT, "audio exceeds MAX_AUDIO_SECONDS")
            return

        if self._asr is not None and self._asr_ready.is_set():
            await self._push_to_asr(data)
        else:
            self._prebuffer.append(data)
            self._prebuffer_bytes += len(data)
            while self._prebuffer_bytes > ASR_PREBUFFER_BYTES:
                dropped = self._prebuffer.popleft()
                self._prebuffer_bytes -= len(dropped)
                self.frames_dropped += 1
                metrics.audio_frames_dropped_total.inc()

    async def handle_audio_end(self, msg: AudioEnd) -> None:
        if self._audio_ended:
            raise ProtocolError(ErrorCode.BAD_PROTOCOL, "audio.end received twice")
        self._audio_ended = True
        self.marks.mark("t3")
        self._transition(SessionState.FINISHING_ASR)

        if self.frames_received == 0 and self.bytes_received == 0:
            await self._fail(ErrorCode.AUDIO_TIMEOUT, "audio.end with no audio")
            return

        # Make sure the ASR start settled (success or error) before finishing.
        if self._asr_start_task is not None:
            try:
                await asyncio.wait_for(
                    asyncio.shield(self._asr_start_task), timeout=self.settings.asr_connect_timeout
                )
            except asyncio.TimeoutError:
                await self._fail(ErrorCode.ASR_CONNECTION_ERROR, "asr start timeout at audio.end")
                return
            except (ProviderError, asyncio.CancelledError):
                pass  # the start task already reported the error
        if self._asr is None or not self._asr_ready.is_set():
            if not self._error_sent and self.state not in TERMINAL_STATES:
                await self._fail(ErrorCode.ASR_CONNECTION_ERROR, "asr not ready at audio.end")
            return

        # Flush the prebuffer, then close the upstream task.
        while self._prebuffer:
            await self._push_to_asr(self._prebuffer.popleft())
        self._prebuffer_bytes = 0
        try:
            await self._asr.finish()
        except ProviderError as exc:
            # A final result may already have arrived through the callback.
            if self._transcript is None:
                await self._fail(exc.code, exc.detail)
                return
        # If finish() produced no final, the provider falls back to the last
        # partial; only a completely silent ASR errors out.
        if self._transcript is None:
            await self._fail(ErrorCode.INTERNAL_ERROR, "no transcript")
            return
        if self.mode == "dictation":
            # Dictation: the transcript is the whole product, no LLM turn.
            await self._finish("success")
            return
        self._start_llm()

    async def handle_cancel(self, msg: SessionCancel) -> None:
        log.info("session cancelled by device", extra={"requestId": self.request_id, "reason": msg.reason})
        await self._cancel("user_cancel")

    # ----------------------------------------------------------------- flows
    async def _start_asr(self) -> None:
        try:
            asr = make_asr_provider(self.settings)
            asr.on_result(self._on_asr_result)
            await asr.start()
            self._asr = asr
            self._asr_ready.set()
            metrics.asr_requests_total.labels(result="ok").inc()
            if self.state == SessionState.RECEIVING_AUDIO:
                self._transition(SessionState.ASR_RUNNING)
            # Flush what arrived while connecting.
            while self._prebuffer:
                await self._push_to_asr(self._prebuffer.popleft())
            self._prebuffer_bytes = 0
        except ProviderError as exc:
            metrics.asr_requests_total.labels(result="error").inc()
            await self._fail(exc.code, exc.detail)
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # never let a provider bug kill the server
            metrics.asr_requests_total.labels(result="error").inc()
            await self._fail(ErrorCode.INTERNAL_ERROR, f"asr start: {exc}")

    async def _push_to_asr(self, data: bytes) -> None:
        assert self._asr is not None
        try:
            await self._asr.push_audio(data)
        except ProviderError as exc:
            await self._fail(exc.code, exc.detail)

    async def _on_asr_result(self, result) -> None:
        if self.state in TERMINAL_STATES:
            return
        self.seq += 1
        if result.is_final:
            self.marks.mark("t5")
            if result.text:
                combined = _join_transcript(self._transcript or "", result.text)
                if len(combined) > MAX_TRANSCRIPT_CHARS:
                    combined = combined[:MAX_TRANSCRIPT_CHARS]
                    log.warning(
                        "transcript capped",
                        extra={"requestId": self.request_id, "cap": MAX_TRANSCRIPT_CHARS},
                    )
                self._transcript = combined
            if self._transcript is not None:
                # Send the accumulated transcript: the device displays this
                # text verbatim and the LLM turn below uses the same string.
                self._send(
                    protocol.asr_final(self.request_id, self.session_id, self._transcript, self.seq)
                )
                log.info(
                    "asr final",
                    extra={"requestId": self.request_id, "seq": self.seq, "chars": len(self._transcript)},
                )
        else:
            self.marks.mark("t4")
            self._send(protocol.asr_partial(self.request_id, self.session_id, result.text, self.seq))

    def _start_llm(self) -> None:
        if self.state in TERMINAL_STATES:
            return
        # Docs section 7 routing: simple questions go straight to the fast
        # LLM; tool/library tasks may route through OpenClaw when enabled.
        if should_route_to_openclaw(self.settings, self._transcript or ""):
            self._llm_task = asyncio.create_task(self._run_openclaw())
        else:
            self._llm_task = asyncio.create_task(self._run_llm())

    async def _run_openclaw(self) -> None:
        try:
            text = await answer_via_openclaw(self.settings, self._transcript or "")
            self._send(protocol.llm_start(self.request_id, self.session_id))
            self.seq += 1
            self._send(protocol.llm_delta(self.request_id, self.session_id, text, self.seq))
            self.marks.mark("t7")
            self.marks.mark("t8")
            self.seq += 1
            self._send(protocol.llm_final(self.request_id, self.session_id, text))
            await self._finish("success")
        except ProviderError as exc:
            await self._fail(exc.code, exc.detail)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            await self._fail(ErrorCode.INTERNAL_ERROR, f"openclaw: {exc}")

    async def _run_llm(self) -> None:
        if not self._transition(SessionState.LLM_RUNNING):
            return
        provider: LlmProvider = make_llm_provider(self.settings)
        first_token = asyncio.Event()
        deltas: list[str] = []

        async def on_delta(piece: str) -> None:
            if not first_token.is_set():
                first_token.set()
                self.marks.mark("t7")
            deltas.append(piece)
            self.seq += 1
            self._send(protocol.llm_delta(self.request_id, self.session_id, piece, self.seq))

        try:
            self.marks.mark("t6")
            self._send(protocol.llm_start(self.request_id, self.session_id))
            stream_task = asyncio.create_task(
                provider.stream(
                    self._transcript or "",
                    self._history,
                    self.settings.max_llm_output_tokens,
                    on_delta,
                )
            )
            first_wait = asyncio.create_task(first_token.wait())
            try:
                done, _pending = await asyncio.wait(
                    {stream_task, first_wait},
                    timeout=self.settings.llm_first_token_timeout,
                    return_when=asyncio.FIRST_COMPLETED,
                )
                if stream_task in done:
                    # Finished (possibly with an error) before any delta.
                    text, usage = stream_task.result()
                elif first_wait in done:
                    text, usage = await asyncio.wait_for(
                        stream_task, timeout=self.settings.upstream_total_timeout
                    )
                else:
                    raise ProviderError(ErrorCode.LLM_CONNECTION_ERROR, "first token timeout")
            finally:
                for task in (stream_task, first_wait):
                    if not task.done():
                        task.cancel()
                for task in (stream_task, first_wait):
                    if not task.done():
                        try:
                            await task
                        except (asyncio.CancelledError, Exception):
                            pass
        except ProviderError as exc:
            metrics.llm_requests_total.labels(result="error").inc()
            await self._fail(exc.code, exc.detail)
            return
        except asyncio.TimeoutError:
            metrics.llm_requests_total.labels(result="error").inc()
            await self._fail(ErrorCode.SESSION_TIMEOUT, "llm total timeout")
            return
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            metrics.llm_requests_total.labels(result="error").inc()
            await self._fail(ErrorCode.INTERNAL_ERROR, f"llm: {exc}")
            return
        finally:
            close = getattr(provider, "close", None)
            if close is not None:
                try:
                    await close()
                except Exception:
                    pass

        self.marks.mark("t8")
        metrics.llm_requests_total.labels(result="ok").inc()
        usage_dict: dict[str, int] = {}
        if usage.input_tokens is not None:
            usage_dict["inputTokens"] = usage.input_tokens
        if usage.output_tokens is not None:
            usage_dict["outputTokens"] = usage.output_tokens
        self.seq += 1
        self._send(protocol.llm_final(self.request_id, self.session_id, text, usage_dict))
        # Keep a short history for follow-ups on the same WS connection.
        self._history.append({"role": "user", "content": self._transcript or ""})
        self._history.append({"role": "assistant", "content": text})
        del self._history[: max(0, len(self._history) - MAX_HISTORY_TURNS * 2)]
        await self._finish("success")

    # ---------------------------------------------------------------- teardown
    async def _finish(self, result: str) -> None:
        if self._finished:
            return
        self._finished = True
        self._result = result
        metrics.voice_sessions_active.dec()
        metrics.voice_sessions_total.labels(result=result).inc()
        self._transition(SessionState.DONE)
        self.marks.mark("t9")
        latency = self.marks.report()
        self._send(protocol.session_done(self.request_id, self.session_id, latency))
        for stage, value in latency.items():
            if value is not None:
                metrics.voice_latency_ms.labels(stage=stage).observe(value)
        log.info(
            "voice_session_finished",
            extra={
                "event": "voice_session_finished",
                "requestId": self.request_id,
                "sessionId": self.session_id,
                "deviceId": self.device_id,
                "result": result,
                "asrFirstPartialMs": latency.get("asrFirstPartial"),
                "asrFinalMs": latency.get("asrFinal"),
                "llmFirstTokenMs": latency.get("llmFirstToken"),
                "llmFinalMs": latency.get("llmFinal"),
                "visibleTotalMs": latency.get("visibleTotal"),
                "audioFrames": self.frames_received,
                "droppedFrames": self.frames_dropped,
            },
        )
        await self._close_providers()
        if self._on_finish is not None:
            self._on_finish(self)

    async def _fail(self, code: ErrorCode, detail: str = "") -> None:
        if self._finished:
            return
        self._finished = True
        self._result = "failed"
        metrics.voice_sessions_active.dec()
        metrics.voice_sessions_total.labels(result="failed").inc()
        self._transition(SessionState.FAILED)
        if not self._error_sent:
            self._error_sent = True
            self.seq += 1
            self._send(protocol.error_event(self.request_id, self.session_id, code, seq=self.seq))
        log.error(
            "voice_session_failed",
            extra={
                "event": "voice_session_failed",
                "requestId": self.request_id,
                "sessionId": self.session_id,
                "deviceId": self.device_id,
                "code": code.value,
                "detail": detail,
                "audioFrames": self.frames_received,
                "droppedFrames": self.frames_dropped,
            },
        )
        await self._close_providers()
        if self._on_finish is not None:
            self._on_finish(self)

    async def _cancel(self, reason: str) -> None:
        if self._finished:
            return
        self._finished = True
        self._result = "cancelled"
        metrics.voice_sessions_active.dec()
        metrics.voice_sessions_total.labels(result="cancelled").inc()
        self._transition(SessionState.CANCELLED)
        log.info(
            "voice_session_cancelled",
            extra={
                "event": "voice_session_cancelled",
                "requestId": self.request_id,
                "sessionId": self.session_id,
                "reason": reason,
            },
        )
        await self._close_providers()
        if self._on_finish is not None:
            self._on_finish(self)

    async def abort(self, reason: str) -> None:
        """Device disconnected or server shutting down: kill all upstream work."""
        await self._cancel(reason)
        metrics.websocket_disconnects_total.labels(reason=reason).inc()

    async def _close_providers(self) -> None:
        # _finish/_fail/_cancel may run inside these very tasks; never cancel
        # or await the task that is currently executing.
        current = asyncio.current_task()
        for task in (self._asr_start_task, self._llm_task):
            if task is not None and task is not current and not task.done():
                task.cancel()
                try:
                    await task
                except (asyncio.CancelledError, Exception):
                    pass
        if self._asr is not None:
            try:
                await self._asr.close()
            except Exception:
                pass
            self._asr = None

    @property
    def finished(self) -> bool:
        return self._finished

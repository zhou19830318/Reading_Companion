"""Alibaba Cloud Model Studio (DashScope) realtime ASR adapter.

Endpoint/contract per the WebSocket API for Paraformer real-time speech
recognition (China Beijing region):

  * connect wss endpoint, header ``Authorization: bearer <api-key>``
  * JSON run-task  -> server ``task-started`` must arrive before any audio
  * binary PCM chunks (s16le / 16 kHz / mono), 100 ms each
  * server ``result-generated`` with payload.output.sentence.text and
    sentence_end (false = partial, true = final sentence)
  * JSON finish-task -> server keeps sending results, then ``task-finished``
  * ``task-failed`` carries header.error_code / header.error_message

The adapter re-chunks the device's 40 ms frames into the upstream chunk size;
this is the only place that knows the vendor's protocol.
"""

from __future__ import annotations

import asyncio
import inspect
import logging
import uuid

import orjson
import websockets
from websockets.exceptions import InvalidStatus, InvalidURI

from ..config import Settings
from ..protocol import ErrorCode
from .base import AsrContract, AsrProvider, AsrResult, ProviderError

log = logging.getLogger("relay.asr.dashscope")

DASHSCOPE_ASR_CONTRACT = AsrContract(
    name="dashscope-paraformer-realtime",
    endpoint="wss://dashscope.aliyuncs.com/api-ws/v1/inference",
    auth="Bearer API key (region-scoped)",
    region="cn-beijing",
    model="paraformer-realtime-v2",
    input_format="pcm_s16le, 16000 Hz, mono",
    chunk_ms=100,
    events={
        "run-task": "start task",
        "task-started": "audio may flow",
        "result-generated": "partial (sentence_end=false) / final (true)",
        "finish-task": "end audio",
        "task-finished": "task complete",
        "task-failed": "mapped to device error",
    },
    heartbeat="none required; result heartbeats ignored",
    reconnect="one connection retry, no audio resume",
)


def _map_task_failure(header: dict) -> ProviderError:
    code = str(header.get("error_code") or header.get("code") or "").lower()
    message = str(header.get("error_message") or header.get("message") or "")
    if "invalid" in code and "key" in code or "authentication" in code or "unauthorized" in code:
        return ProviderError(ErrorCode.ASR_AUTH_ERROR, f"task-failed: {code} {message}")
    if "throttl" in code or "rate" in code or "limit" in code:
        return ProviderError(ErrorCode.ASR_RATE_LIMITED, f"task-failed: {code} {message}")
    return ProviderError(ErrorCode.ASR_CONNECTION_ERROR, f"task-failed: {code} {message}")


async def _connect(endpoint: str, headers: dict[str, str], timeout: float):
    """websockets>=13 compat: extra_headers was renamed additional_headers."""
    kwargs = {"open_timeout": timeout, "max_size": 1 << 20, "close_timeout": 2}
    params = inspect.signature(websockets.connect).parameters
    if "additional_headers" in params:
        kwargs["additional_headers"] = headers
    else:  # pragma: no cover - older websockets
        kwargs["extra_headers"] = headers
    return await websockets.connect(endpoint, **kwargs)


class DashScopeAsrProvider(AsrProvider):
    contract = DASHSCOPE_ASR_CONTRACT

    def __init__(self, settings: Settings):
        super().__init__()
        self.settings = settings
        self._ws = None
        self._recv_task: asyncio.Task | None = None
        self._task_id = uuid.uuid4().hex
        self._started_evt = asyncio.Event()
        self._finished_evt = asyncio.Event()
        self._error: ProviderError | None = None
        self._final_text: str | None = None
        self._last_text = ""
        self._chunk_bytes = settings.asr_chunk_ms * 16 * 2
        self._pending = bytearray()
        self._closed = False

    # ------------------------------------------------------------- lifecycle
    async def start(self) -> None:
        api_key = self.settings.dashscope_api_key
        if not api_key:
            raise ProviderError(ErrorCode.ASR_AUTH_ERROR, "DASHSCOPE_API_KEY not configured")
        headers = {
            "Authorization": f"bearer {api_key}",
            "X-DashScope-DataInspection": "enable",
        }
        if self.settings.dashscope_workspace_id:
            headers["X-DashScope-WorkSpace"] = self.settings.dashscope_workspace_id
        try:
            self._ws = await _connect(
                self.settings.dashscope_asr_endpoint(), headers, self.settings.asr_connect_timeout
            )
        except InvalidStatus as exc:
            status = exc.response.status_code
            if status in (401, 403):
                raise ProviderError(ErrorCode.ASR_AUTH_ERROR, f"http {status}") from exc
            raise ProviderError(ErrorCode.ASR_CONNECTION_ERROR, f"http {status}") from exc
        except (OSError, asyncio.TimeoutError, InvalidURI) as exc:
            raise ProviderError(ErrorCode.ASR_CONNECTION_ERROR, f"connect: {exc}") from exc

        self._recv_task = asyncio.create_task(self._recv_loop())
        run_task = {
            "header": {
                "action": "run-task",
                "task_id": self._task_id,
                "streaming": "duplex",
            },
            "payload": {
                "task_group": "audio",
                "task": "asr",
                "function": "recognition",
                "model": self.settings.asr_model,
                "parameters": {
                    "format": "pcm",
                    "sample_rate": 16000,
                    "disfluency_removal_enabled": False,
                    "language_hints": self.settings.language_hints,
                },
                "input": {},
            },
        }
        await self._ws.send(orjson.dumps(run_task).decode())
        try:
            await asyncio.wait_for(self._started_evt.wait(), timeout=self.settings.asr_task_started_timeout)
        except asyncio.TimeoutError as exc:
            raise ProviderError(ErrorCode.ASR_CONNECTION_ERROR, "task-started timeout") from exc
        if self._error is not None:
            raise self._error
        log.info("asr task started", extra={"task_id": self._task_id, "model": self.settings.asr_model})

    async def push_audio(self, pcm: bytes) -> None:
        if self._error is not None:
            raise self._error
        self._pending.extend(pcm)
        while len(self._pending) >= self._chunk_bytes:
            chunk = bytes(self._pending[: self._chunk_bytes])
            del self._pending[: self._chunk_bytes]
            await self._ws.send(chunk)

    async def finish(self) -> None:
        if self._pending:
            await self._ws.send(bytes(self._pending))
            self._pending.clear()
        finish_task = {
            "header": {
                "action": "finish-task",
                "task_id": self._task_id,
                "streaming": "duplex",
            },
            "payload": {"input": {}},
        }
        try:
            await self._ws.send(orjson.dumps(finish_task).decode())
        except Exception as exc:  # socket already gone
            raise ProviderError(ErrorCode.ASR_CONNECTION_ERROR, f"finish send: {exc}") from exc
        try:
            await asyncio.wait_for(self._finished_evt.wait(), timeout=self.settings.asr_finish_timeout)
        except asyncio.TimeoutError:
            if not self._last_text:
                # No task-finished and no results at all: upstream stalled.
                # Report a retryable transport failure instead of an
                # internal error so the device can fall back to legacy.
                raise ProviderError(ErrorCode.ASR_CONNECTION_ERROR, "task-finished timeout, no results")
            log.warning("asr task-finished timeout; using last result", extra={"task_id": self._task_id})
        if self._error is not None:
            raise self._error
        # finish-task 后 final result 仍能到达 (docs acceptance list); if the
        # provider only produced partials, promote the last one so the device
        # still gets an asr.final.
        if not self._final_text and self._last_text:
            await self._emit(AsrResult(text=self._last_text, is_final=True))

    async def close(self) -> None:
        self._closed = True
        if self._recv_task is not None:
            self._recv_task.cancel()
            try:
                await self._recv_task
            except (asyncio.CancelledError, Exception):
                pass
            self._recv_task = None
        if self._ws is not None:
            try:
                await self._ws.close()
            except Exception:
                pass
            self._ws = None

    # ---------------------------------------------------------------- inside
    async def _recv_loop(self) -> None:
        assert self._ws is not None
        try:
            async for raw in self._ws:
                if isinstance(raw, (bytes, bytearray)):
                    continue
                try:
                    msg = orjson.loads(raw)
                except orjson.JSONDecodeError:
                    continue
                header = msg.get("header") or {}
                event = header.get("event")
                if event == "task-started":
                    self._started_evt.set()
                elif event == "result-generated":
                    await self._on_result_generated(msg)
                elif event == "task-finished":
                    self._finished_evt.set()
                elif event == "task-failed":
                    self._error = _map_task_failure(header)
                    log.error("asr task failed", extra={"error_code": header.get("error_code", "")})
                    self._started_evt.set()
                    self._finished_evt.set()
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # connection dropped mid-task
            if not self._closed:
                self._error = ProviderError(ErrorCode.ASR_CONNECTION_ERROR, f"recv: {exc}")
                self._started_evt.set()
                self._finished_evt.set()

    async def _on_result_generated(self, msg: dict) -> None:
        payload = msg.get("payload") or {}
        sentence = ((payload.get("output") or {}).get("sentence")) or {}
        if sentence.get("heartbeat"):
            return
        text = str(sentence.get("text") or "")
        if not text:
            return
        is_final = bool(sentence.get("sentence_end"))
        if is_final:
            self._final_text = text
        elif text == self._last_text:
            return  # repeated intermediate result; drop duplicates
        self._last_text = text
        await self._emit(
            AsrResult(
                text=text,
                is_final=is_final,
                begin_ms=sentence.get("begin_time"),
                end_ms=sentence.get("end_time"),
            )
        )

"""Device protocol: event models, error codes and frame builders.

Wire format (one WebSocket, JSON control frames + raw binary PCM frames):

  device -> relay: session.start, audio.start, <binary PCM frames>, audio.end,
                   session.cancel
  relay  -> device: session.started, asr.partial, asr.final, llm.start,
                    llm.delta, llm.final, session.done, error

PCM contract: 16000 Hz, mono, signed 16-bit little-endian, 40 ms frames
(1280 bytes). The relay accepts any whole-frame size and re-chunks for the
upstream provider; the declared format is validated, never guessed.
"""

from __future__ import annotations

import time
import uuid
from enum import Enum
from typing import Any, Literal

import orjson
from pydantic import BaseModel, Field, ValidationError, field_validator

PROTOCOL_VERSION = 1

AUDIO_SAMPLE_RATE = 16000
AUDIO_CHANNELS = 1
AUDIO_FORMAT = "pcm_s16le"
AUDIO_FRAME_MS = 40
AUDIO_FRAME_BYTES = AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS // 1000 * 2  # 1280

# session.start modes: "relay" runs ASR + LLM, "dictation" stops at the
# transcript (voice bookmark / in-keyboard dictation).
SESSION_MODES = frozenset({"relay", "dictation"})


class ErrorCode(str, Enum):
    BAD_PROTOCOL = "BAD_PROTOCOL"
    UNAUTHORIZED = "UNAUTHORIZED"
    DEVICE_NOT_ALLOWED = "DEVICE_NOT_ALLOWED"
    AUDIO_FORMAT_UNSUPPORTED = "AUDIO_FORMAT_UNSUPPORTED"
    AUDIO_TIMEOUT = "AUDIO_TIMEOUT"
    ASR_CONNECTION_ERROR = "ASR_CONNECTION_ERROR"
    ASR_AUTH_ERROR = "ASR_AUTH_ERROR"
    ASR_RATE_LIMITED = "ASR_RATE_LIMITED"
    LLM_CONNECTION_ERROR = "LLM_CONNECTION_ERROR"
    LLM_AUTH_ERROR = "LLM_AUTH_ERROR"
    LLM_RATE_LIMITED = "LLM_RATE_LIMITED"
    SESSION_TIMEOUT = "SESSION_TIMEOUT"
    SERVER_OVERLOADED = "SERVER_OVERLOADED"
    INTERNAL_ERROR = "INTERNAL_ERROR"


# User-visible, Chinese, never carries upstream detail (docs section 4.3).
ERROR_MESSAGES: dict[ErrorCode, str] = {
    ErrorCode.BAD_PROTOCOL: "设备事件或协议版本错误",
    ErrorCode.UNAUTHORIZED: "语音服务认证失败",
    ErrorCode.DEVICE_NOT_ALLOWED: "设备未注册",
    ErrorCode.AUDIO_FORMAT_UNSUPPORTED: "音频格式不支持",
    ErrorCode.AUDIO_TIMEOUT: "音频接收超时",
    ErrorCode.ASR_CONNECTION_ERROR: "语音识别服务连接失败",
    ErrorCode.ASR_AUTH_ERROR: "语音识别服务认证失败",
    ErrorCode.ASR_RATE_LIMITED: "语音识别服务繁忙",
    ErrorCode.LLM_CONNECTION_ERROR: "回答服务连接失败",
    ErrorCode.LLM_AUTH_ERROR: "回答服务认证失败",
    ErrorCode.LLM_RATE_LIMITED: "回答服务繁忙",
    ErrorCode.SESSION_TIMEOUT: "会话超时",
    ErrorCode.SERVER_OVERLOADED: "服务器繁忙，请稍后再试",
    ErrorCode.INTERNAL_ERROR: "内部错误",
}

# docs section 17.5.3 table.
RETRYABLE: dict[ErrorCode, bool] = {
    ErrorCode.BAD_PROTOCOL: False,
    ErrorCode.UNAUTHORIZED: False,
    ErrorCode.DEVICE_NOT_ALLOWED: False,
    ErrorCode.AUDIO_FORMAT_UNSUPPORTED: False,
    ErrorCode.AUDIO_TIMEOUT: False,
    ErrorCode.ASR_CONNECTION_ERROR: True,
    ErrorCode.ASR_AUTH_ERROR: False,
    ErrorCode.ASR_RATE_LIMITED: True,
    ErrorCode.LLM_CONNECTION_ERROR: True,
    ErrorCode.LLM_AUTH_ERROR: False,
    ErrorCode.LLM_RATE_LIMITED: True,
    ErrorCode.SESSION_TIMEOUT: False,
    ErrorCode.SERVER_OVERLOADED: True,
    ErrorCode.INTERNAL_ERROR: False,
}


def utc_now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def new_id() -> str:
    return uuid.uuid4().hex


# --------------------------------------------------------------------------
# Device -> relay events
# --------------------------------------------------------------------------


class AudioParams(BaseModel):
    sampleRate: int
    channels: int
    format: str
    frameMs: int = AUDIO_FRAME_MS


class SessionStart(BaseModel):
    type: Literal["session.start"]
    requestId: str
    deviceId: str = ""
    audio: AudioParams
    mode: str = "relay"

    @field_validator("requestId")
    @classmethod
    def _req_id(cls, v: str) -> str:
        v = v.strip()
        if not v or len(v) > 64:
            raise ValueError("requestId")
        return v

    @field_validator("mode")
    @classmethod
    def _mode(cls, v: str) -> str:
        if v not in SESSION_MODES:
            raise ValueError("mode")
        return v

    def audio_supported(self) -> bool:
        return (
            self.audio.sampleRate == AUDIO_SAMPLE_RATE
            and self.audio.channels == AUDIO_CHANNELS
            and self.audio.format == AUDIO_FORMAT
        )


class AudioStart(BaseModel):
    type: Literal["audio.start"] = "audio.start"
    requestId: str
    sessionId: str


class AudioEnd(BaseModel):
    type: Literal["audio.end"] = "audio.end"
    requestId: str
    sessionId: str
    reason: str = "vad_silence"


class SessionCancel(BaseModel):
    type: Literal["session.cancel"] = "session.cancel"
    requestId: str
    sessionId: str = ""
    reason: str = "user_cancel"


DEVICE_EVENTS = {
    "session.start": SessionStart,
    "audio.start": AudioStart,
    "audio.end": AudioEnd,
    "session.cancel": SessionCancel,
}


def parse_device_event(raw: bytes | str) -> BaseModel:
    """Parse one control frame. Raises ProtocolError (BAD_PROTOCOL)."""
    try:
        msg = orjson.loads(raw)
    except orjson.JSONDecodeError as exc:
        raise ProtocolError(ErrorCode.BAD_PROTOCOL, "invalid JSON") from exc
    if not isinstance(msg, dict):
        raise ProtocolError(ErrorCode.BAD_PROTOCOL, "not an object")
    etype = msg.get("type")
    if etype not in DEVICE_EVENTS:
        raise ProtocolError(ErrorCode.BAD_PROTOCOL, f"unknown type {etype!r}")
    try:
        return DEVICE_EVENTS[etype].model_validate(msg)
    except ValidationError as exc:
        raise ProtocolError(ErrorCode.BAD_PROTOCOL, f"bad {etype}") from exc


class ProtocolError(Exception):
    def __init__(self, code: ErrorCode, detail: str = ""):
        super().__init__(detail)
        self.code = code
        self.detail = detail


# --------------------------------------------------------------------------
# relay -> device events
# --------------------------------------------------------------------------


def _dumps(obj: dict[str, Any]) -> str:
    return orjson.dumps(obj).decode()


def session_started(request_id: str, session_id: str) -> str:
    return _dumps(
        {
            "type": "session.started",
            "requestId": request_id,
            "sessionId": session_id,
            "serverTime": utc_now_iso(),
        }
    )


def asr_partial(request_id: str, session_id: str, text: str, seq: int) -> str:
    return _dumps(
        {
            "type": "asr.partial",
            "requestId": request_id,
            "sessionId": session_id,
            "text": text,
            "seq": seq,
        }
    )


def asr_final(request_id: str, session_id: str, text: str, seq: int) -> str:
    return _dumps(
        {
            "type": "asr.final",
            "requestId": request_id,
            "sessionId": session_id,
            "text": text,
            "seq": seq,
        }
    )


def llm_start(request_id: str, session_id: str) -> str:
    return _dumps({"type": "llm.start", "requestId": request_id, "sessionId": session_id})


def llm_delta(request_id: str, session_id: str, text: str, seq: int) -> str:
    return _dumps(
        {
            "type": "llm.delta",
            "requestId": request_id,
            "sessionId": session_id,
            "text": text,
            "seq": seq,
        }
    )


def llm_final(
    request_id: str,
    session_id: str,
    text: str,
    usage: dict[str, int] | None = None,
) -> str:
    out: dict[str, Any] = {
        "type": "llm.final",
        "requestId": request_id,
        "sessionId": session_id,
        "text": text,
    }
    if usage:
        out["usage"] = usage
    return _dumps(out)


def session_done(request_id: str, session_id: str, latency_ms: dict[str, int | None]) -> str:
    return _dumps(
        {
            "type": "session.done",
            "requestId": request_id,
            "sessionId": session_id,
            "latencyMs": {k: v for k, v in latency_ms.items() if v is not None},
        }
    )


def error_event(
    request_id: str,
    session_id: str,
    code: ErrorCode,
    message: str | None = None,
    seq: int | None = None,
) -> str:
    out: dict[str, Any] = {
        "type": "error",
        "requestId": request_id,
        "sessionId": session_id,
        "code": code.value,
        "message": message if message is not None else ERROR_MESSAGES[code],
        "retryable": RETRYABLE[code],
    }
    if seq is not None:
        out["seq"] = seq
    return _dumps(out)

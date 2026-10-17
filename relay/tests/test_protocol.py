"""Protocol codec tests: parsing, error codes, frame builders."""

from __future__ import annotations

import json

import pytest

from relay import protocol
from relay.protocol import (
    AudioEnd,
    AudioStart,
    ErrorCode,
    ProtocolError,
    SessionCancel,
    SessionStart,
    parse_device_event,
)


def test_parse_session_start():
    raw = json.dumps(
        {
            "type": "session.start",
            "requestId": "abc123",
            "deviceId": "reading-companion-001",
            "audio": {"sampleRate": 16000, "channels": 1, "format": "pcm_s16le", "frameMs": 40},
            "mode": "relay",
        }
    )
    event = parse_device_event(raw)
    assert isinstance(event, SessionStart)
    assert event.audio_supported()
    assert event.deviceId == "reading-companion-001"


def test_parse_session_start_dictation_mode():
    raw = json.dumps(
        {
            "type": "session.start",
            "requestId": "abc",
            "audio": {"sampleRate": 16000, "channels": 1, "format": "pcm_s16le", "frameMs": 40},
            "mode": "dictation",
        }
    )
    event = parse_device_event(raw)
    assert isinstance(event, SessionStart)
    assert event.mode == "dictation"


def test_parse_session_start_rejects_unknown_mode():
    raw = json.dumps(
        {
            "type": "session.start",
            "requestId": "abc",
            "audio": {"sampleRate": 16000, "channels": 1, "format": "pcm_s16le", "frameMs": 40},
            "mode": "sing",
        }
    )
    with pytest.raises(ProtocolError) as exc:
        parse_device_event(raw)
    assert exc.value.code == ErrorCode.BAD_PROTOCOL


def test_parse_session_start_rejects_bad_audio():
    raw = json.dumps(
        {
            "type": "session.start",
            "requestId": "abc",
            "audio": {"sampleRate": 8000, "channels": 2, "format": "mp3", "frameMs": 40},
        }
    )
    event = parse_device_event(raw)
    assert not event.audio_supported()


@pytest.mark.parametrize(
    "raw",
    ["not json", "[]", '{"type":"nope"}', '{"type":"audio.start"}', '{"type":"audio.end","requestId":"x"}'],
)
def test_parse_rejects_bad_frames(raw):
    with pytest.raises(ProtocolError) as exc:
        parse_device_event(raw)
    assert exc.value.code == ErrorCode.BAD_PROTOCOL


def test_parse_audio_events():
    start = parse_device_event('{"type":"audio.start","requestId":"r1","sessionId":"s1"}')
    assert isinstance(start, AudioStart)
    end = parse_device_event('{"type":"audio.end","requestId":"r1","sessionId":"s1","reason":"vad_silence"}')
    assert isinstance(end, AudioEnd)
    cancel = parse_device_event('{"type":"session.cancel","requestId":"r1","sessionId":"s1"}')
    assert isinstance(cancel, SessionCancel)


def test_error_event_shape_and_retryable():
    raw = protocol.error_event("r1", "s1", ErrorCode.UNAUTHORIZED, seq=3)
    msg = json.loads(raw)
    assert msg == {
        "type": "error",
        "requestId": "r1",
        "sessionId": "s1",
        "code": "UNAUTHORIZED",
        "message": "语音服务认证失败",
        "retryable": False,
        "seq": 3,
    }
    retryable = json.loads(protocol.error_event("r", "s", ErrorCode.ASR_CONNECTION_ERROR))
    assert retryable["retryable"] is True
    assert retryable["message"] == "语音识别服务连接失败"


def test_error_messages_do_not_leak_upstream_detail():
    """Every device-visible message is our fixed user text, never provider text."""
    for code, message in protocol.ERROR_MESSAGES.items():
        assert "key" not in message.lower()
        assert "http" not in message.lower()
        assert "dashscope" not in message.lower()


def test_frame_builders():
    started = json.loads(protocol.session_started("r1", "s1"))
    assert started["type"] == "session.started"
    assert started["requestId"] == "r1"
    assert "serverTime" in started

    partial = json.loads(protocol.asr_partial("r1", "s1", "今天", 12))
    assert partial == {
        "type": "asr.partial",
        "requestId": "r1",
        "sessionId": "s1",
        "text": "今天",
        "seq": 12,
    }

    final = json.loads(protocol.llm_final("r1", "s1", "今天适合阅读。", {"inputTokens": 1, "outputTokens": 2}))
    assert final["type"] == "llm.final"
    assert final["usage"] == {"inputTokens": 1, "outputTokens": 2}

    done = json.loads(protocol.session_done("r1", "s1", {"connect": 10, "asrFinal": None}))
    assert done["latencyMs"] == {"connect": 10}


def test_audio_frame_contract_constants():
    assert protocol.AUDIO_SAMPLE_RATE == 16000
    assert protocol.AUDIO_FRAME_MS == 40
    assert protocol.AUDIO_FRAME_BYTES == 1280

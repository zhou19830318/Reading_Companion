"""Session state machine tests (mock providers, no network)."""

from __future__ import annotations

import asyncio
import json

import pytest

from relay import protocol
from relay.config import Settings
from relay.protocol import AudioEnd, AudioStart, SessionCancel, SessionStart
from relay.providers.base import AsrResult
from relay.session import SessionState, VoiceSession


def make_start(**overrides) -> SessionStart:
    data = {
        "type": "session.start",
        "requestId": "req-1",
        "deviceId": "debug-client",
        "audio": {"sampleRate": 16000, "channels": 1, "format": "pcm_s16le", "frameMs": 40},
        "mode": "relay",
    }
    data.update(overrides)
    return SessionStart.model_validate(data)


class Harness:
    def __init__(self, **env):
        values = {
            "ASR_PROVIDER": "mock",
            "LLM_PROVIDER": "mock",
            "MOCK_ASR_PARTIAL_INTERVAL_MS": "100",
            "MOCK_ASR_FINAL_DELAY_MS": "10",
            "MOCK_LLM_DELTA_INTERVAL_MS": "1",
            "RELAY_DEVICE_TOKEN": "t",
        }
        values.update(env)
        self.settings = Settings(**values)
        self.frames: list[dict] = []
        self.session: VoiceSession | None = None

    def send(self, frame: str) -> None:
        self.frames.append(json.loads(frame))

    async def new_session(self, **overrides) -> VoiceSession:
        self.session = VoiceSession(self.settings, self.send, make_start(**overrides), None)
        await self.session.handle_session_start()
        return self.session

    def types(self) -> list[str]:
        return [f["type"] for f in self.frames]


def frame(ms: int = 40) -> bytes:
    """ms of 16 kHz mono s16le audio (16 samples per ms, 2 bytes each)."""
    return b"\x01\x02" * (16 * ms)


@pytest.mark.asyncio
async def test_happy_path_mock():
    h = Harness()
    s = await h.new_session()
    await s.handle_audio_start(AudioStart(requestId="req-1", sessionId=s.session_id))
    for _ in range(25):  # 1 s of audio
        await s.handle_binary(frame(40))
    await s.handle_audio_end(AudioEnd(requestId="req-1", sessionId=s.session_id))
    assert s._llm_task is not None
    await s._llm_task
    types = h.types()
    assert types[0] == "session.started"
    assert "asr.partial" in types
    assert "asr.final" in types
    assert "llm.start" in types
    assert "llm.delta" in types
    assert types[-1] == "session.done"
    assert s.state == SessionState.DONE
    done = h.frames[-1]
    assert "asrFirstPartial" in done["latencyMs"]
    assert s.frames_received == 25


@pytest.mark.asyncio
async def test_multi_sentence_finals_accumulate_across_the_capture():
    h = Harness()
    s = await h.new_session()
    await s.handle_audio_start(AudioStart(requestId="req-1", sessionId=s.session_id))
    await s.handle_binary(frame(40))
    await s._on_asr_result(AsrResult(text="今天天气怎么样", is_final=True))
    await s._on_asr_result(AsrResult(text="适合出门吗", is_final=True))
    finals = [f["text"] for f in h.frames if f["type"] == "asr.final"]
    assert finals == ["今天天气怎么样", "今天天气怎么样适合出门吗"]
    assert s._transcript == "今天天气怎么样适合出门吗"


@pytest.mark.asyncio
async def test_ascii_sentence_finals_join_with_a_space():
    h = Harness()
    s = await h.new_session()
    await s.handle_audio_start(AudioStart(requestId="req-1", sessionId=s.session_id))
    await s.handle_binary(frame(40))
    await s._on_asr_result(AsrResult(text="where is", is_final=True))
    await s._on_asr_result(AsrResult(text="the library", is_final=True))
    assert s._transcript == "where is the library"


@pytest.mark.asyncio
async def test_dictation_finishes_at_transcript_without_llm():
    h = Harness()
    s = await h.new_session(mode="dictation")
    await s.handle_audio_start(AudioStart(requestId="req-1", sessionId=s.session_id))
    for _ in range(25):  # 1 s of audio
        await s.handle_binary(frame(40))
    await s.handle_audio_end(AudioEnd(requestId="req-1", sessionId=s.session_id))
    types = h.types()
    assert types[0] == "session.started"
    assert "asr.final" in types
    assert "llm.start" not in types
    assert "llm.delta" not in types
    assert types[-1] == "session.done"
    assert s.state == SessionState.DONE
    assert s._llm_task is None
    assert s.frames_received == 25


@pytest.mark.asyncio
async def test_audio_end_twice_is_protocol_error():
    h = Harness()
    s = await h.new_session()
    await s.handle_audio_start(AudioStart(requestId="req-1", sessionId=s.session_id))
    await s.handle_binary(frame(40))
    await s.handle_audio_end(AudioEnd(requestId="req-1", sessionId=s.session_id))
    with pytest.raises(protocol.ProtocolError):
        await s.handle_audio_end(AudioEnd(requestId="req-1", sessionId=s.session_id))


@pytest.mark.asyncio
async def test_audio_end_with_no_audio_fails_with_audio_timeout():
    h = Harness()
    s = await h.new_session()
    await s.handle_audio_start(AudioStart(requestId="req-1", sessionId=s.session_id))
    await s.handle_audio_end(AudioEnd(requestId="req-1", sessionId=s.session_id))
    assert s.state == SessionState.FAILED
    assert h.frames[-1]["type"] == "error"
    assert h.frames[-1]["code"] == "AUDIO_TIMEOUT"


@pytest.mark.asyncio
async def test_max_audio_seconds_enforced():
    h = Harness(MAX_AUDIO_SECONDS="1")
    s = await h.new_session()
    await s.handle_audio_start(AudioStart(requestId="req-1", sessionId=s.session_id))
    for _ in range(30):  # 1.2 s
        await s.handle_binary(frame(40))
    assert s.state == SessionState.FAILED
    assert h.frames[-1]["code"] == "AUDIO_TIMEOUT"


@pytest.mark.asyncio
async def test_cancel_stops_pipeline():
    h = Harness()
    s = await h.new_session()
    await s.handle_audio_start(AudioStart(requestId="req-1", sessionId=s.session_id))
    await s.handle_binary(frame(40))
    await s.handle_cancel(SessionCancel(requestId="req-1", sessionId=s.session_id))
    assert s.state == SessionState.CANCELLED
    assert s.finished
    # Frames after cancel are dropped, not fed upstream.
    before = s.frames_dropped
    await s.handle_binary(frame(40))
    assert s.frames_dropped == before + 1


@pytest.mark.asyncio
async def test_asr_start_failure_emits_error():
    h = Harness(MOCK_ASR_FAIL_START="true")
    s = await h.new_session()
    await s.handle_audio_start(AudioStart(requestId="req-1", sessionId=s.session_id))
    await s.handle_binary(frame(40))
    await asyncio.sleep(0.05)
    assert s.state == SessionState.FAILED
    assert h.frames[-1]["code"] == "ASR_CONNECTION_ERROR"


@pytest.mark.asyncio
async def test_llm_failure_after_asr_final():
    h = Harness(MOCK_LLM_FAIL="true")
    s = await h.new_session()
    await s.handle_audio_start(AudioStart(requestId="req-1", sessionId=s.session_id))
    await s.handle_binary(frame(40))
    await s.handle_audio_end(AudioEnd(requestId="req-1", sessionId=s.session_id))
    assert s._llm_task is not None
    await s._llm_task
    assert s.state == SessionState.FAILED
    assert h.frames[-1]["code"] == "LLM_CONNECTION_ERROR"
    # asr.final still made it out before the LLM failed.
    assert "asr.final" in h.types()


@pytest.mark.asyncio
async def test_prebuffer_drops_oldest_beyond_limit():
    from relay import session as session_mod

    h = Harness()
    s = await h.new_session()
    # No audio.start: the ASR start task never runs, so frames land in the
    # prebuffer and the oldest are dropped once the byte cap is passed.
    big = frame(40)
    n = session_mod.ASR_PREBUFFER_BYTES // len(big) + 10
    for _ in range(n):
        await s.handle_binary(big)
    assert s.frames_dropped >= 10


@pytest.mark.asyncio
async def test_transition_table_rejects_illegal_moves():
    from relay.session import _TRANSITIONS

    assert SessionState.SESSION_STARTED not in _TRANSITIONS[SessionState.LLM_RUNNING]
    assert SessionState.DONE in _TRANSITIONS[SessionState.LLM_RUNNING]
    assert SessionState.CANCELLED in _TRANSITIONS[SessionState.RECEIVING_AUDIO]

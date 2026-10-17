"""DashScope ASR result parsing: partial vs final, dedup, heartbeat.

Regression guard: the first real-credential contract run produced only
asr.final because the partial branch of ``_on_result_generated`` was missing.
"""

from __future__ import annotations

import pytest

from relay.config import Settings
from relay.providers.asr_dashscope import DashScopeAsrProvider
from relay.providers.base import ProviderError


def settings(**env) -> Settings:
    return Settings(
        ASR_PROVIDER="px",
        LLM_PROVIDER="mock",
        DASHSCOPE_API_KEY="test-key",
        DASHSCOPE_WORKSPACE_ID="ws-test",
        **env,
    )


def sentence_msg(text: str, *, sentence_end: bool, heartbeat: bool = False, begin: int = 0, end: int = 0) -> dict:
    sentence: dict = {"text": text, "sentence_end": sentence_end}
    if heartbeat:
        sentence = {"heartbeat": True}
    else:
        sentence["begin_time"] = begin
        sentence["end_time"] = end
    return {"header": {"event": "result-generated"}, "payload": {"output": {"sentence": sentence}}}


async def collect(msgs: list[dict]) -> list:
    provider = DashScopeAsrProvider(settings())
    results = []
    provider.on_result(lambda r: _record(results, r))
    for msg in msgs:
        await provider._on_result_generated(msg)
    return results


async def _record(results: list, r):
    results.append(r)


@pytest.mark.asyncio
async def test_partial_then_final_are_emitted_in_order():
    results = await collect(
        [
            sentence_msg("你好", sentence_end=False, begin=80, end=560),
            sentence_msg("你好，请帮我", sentence_end=False, begin=80, end=900),
            sentence_msg("你好，请帮我推荐一本书。", sentence_end=True, begin=80, end=2500),
        ]
    )
    assert [r.text for r in results] == ["你好", "你好，请帮我", "你好，请帮我推荐一本书。"]
    assert [r.is_final for r in results] == [False, False, True]
    assert results[0].begin_ms == 80 and results[0].end_ms == 560


@pytest.mark.asyncio
async def test_repeated_intermediate_is_deduped_final_always_emitted():
    results = await collect(
        [
            sentence_msg("你好", sentence_end=False),
            sentence_msg("你好", sentence_end=False),
            sentence_msg("你好", sentence_end=True),
        ]
    )
    assert [r.is_final for r in results] == [False, True]


@pytest.mark.asyncio
async def test_heartbeat_and_empty_text_are_ignored():
    results = await collect(
        [
            sentence_msg("", sentence_end=False, heartbeat=True),
            sentence_msg("", sentence_end=False),
            sentence_msg("再见", sentence_end=True),
        ]
    )
    assert [r.text for r in results] == ["再见"]


@pytest.mark.asyncio
async def test_finish_promotes_last_partial_when_no_final_arrived():
    provider = DashScopeAsrProvider(settings())
    results = []
    provider.on_result(lambda r: _record(results, r))
    await provider._on_result_generated(sentence_msg("只剩部分结果", sentence_end=False))
    provider._finished_evt.set()
    provider._ws = _FakeWs()
    await provider.finish()
    assert [r.is_final for r in results] == [False, True]
    assert results[-1].text == "只剩部分结果"


class _FakeWs:
    async def send(self, _data):
        return None


@pytest.mark.asyncio
async def test_finish_timeout_without_any_result_is_retryable_connection_error():
    provider = DashScopeAsrProvider(settings(ASR_FINISH_TIMEOUT=0.01))
    provider._ws = _FakeWs()
    with pytest.raises(ProviderError) as exc:
        await provider.finish()
    assert exc.value.code.value == "ASR_CONNECTION_ERROR"

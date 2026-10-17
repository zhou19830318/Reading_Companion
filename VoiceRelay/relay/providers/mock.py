"""Mock providers for development (docs phase 1: local echo/mock relay).

They let the full protocol, device state machine, fallback and metrics be
verified with zero cloud credentials. Timing knobs simulate upstream latency.
"""

from __future__ import annotations

import asyncio

from ..config import Settings
from ..protocol import ErrorCode
from .base import AsrContract, AsrProvider, AsrResult, LlmProvider, LlmUsage, ProviderError

MOCK_ASR_CONTRACT = AsrContract(
    name="mock-asr",
    endpoint="in-process",
    auth="none",
    region="local",
    model="mock",
    events={
        "partial": "result-generated(sentence_end=false)",
        "final": "result-generated(sentence_end=true)",
    },
    chunk_ms=100,
    heartbeat="none",
    reconnect="none",
)


class MockAsrProvider(AsrProvider):
    contract = MOCK_ASR_CONTRACT

    def __init__(self, settings: Settings):
        super().__init__()
        self.settings = settings
        self._partials = [p for p in settings.mock_asr_partials.split(",") if p]
        self._next_partial_ms = settings.mock_asr_partial_interval_ms
        self._next_partial = 0
        self._audio_ms = 0.0
        self._task: asyncio.Task | None = None

    async def start(self) -> None:
        if self.settings.mock_asr_fail_start:
            raise ProviderError(ErrorCode.ASR_CONNECTION_ERROR, "mock start failure")
        await asyncio.sleep(0)  # yield: mirrors a real async connect

    async def push_audio(self, pcm: bytes) -> None:
        self._audio_ms += len(pcm) / 2 / 16.0  # 16 samples/ms
        if self._next_partial < len(self._partials) and self._audio_ms >= self._next_partial_ms:
            self._next_partial += 1
            await self._emit(AsrResult(text=self._partials[self._next_partial - 1], is_final=False))

    async def finish(self) -> None:
        await asyncio.sleep(self.settings.mock_asr_final_delay_ms / 1000.0)
        text = self.settings.mock_asr_text
        # If configured partials already ended on this text, upgrading the
        # final still comes through; the device dedupes by seq.
        await self._emit(AsrResult(text=text, is_final=True))

    async def close(self) -> None:
        if self._task is not None:
            self._task.cancel()
            self._task = None


class MockLlmProvider(LlmProvider):
    contract = AsrContract(
        name="mock-llm",
        endpoint="in-process",
        auth="none",
        region="local",
        model="mock",
        input_format="chat",
        chunk_ms=0,
        events={"delta": "OpenAI SSE delta", "final": "OpenAI SSE [DONE]"},
        heartbeat="none",
        reconnect="none",
    )

    def __init__(self, settings: Settings):
        super().__init__()
        self.settings = settings

    async def stream(
        self,
        prompt: str,
        history: list[dict[str, str]],
        max_tokens: int,
        on_delta,
        system: str | None = None,
    ) -> tuple[str, LlmUsage]:
        if self.settings.mock_llm_fail:
            raise ProviderError(ErrorCode.LLM_CONNECTION_ERROR, "mock llm failure")
        text = self.settings.mock_llm_text
        width = max(1, self.settings.mock_llm_delta_chars)
        delay = self.settings.mock_llm_delta_interval_ms / 1000.0
        out: list[str] = []
        for i in range(0, len(text), width):
            piece = text[i : i + width]
            out.append(piece)
            await asyncio.sleep(delay)
            await on_delta(piece)
        full = "".join(out)
        return full, LlmUsage(input_tokens=len(prompt), output_tokens=len(full))

"""Provider contracts.

Every adapter declares its endpoint, auth, region, model, input format, chunk
duration and event mapping here — the device never sees any of it (docs
section 7).
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Awaitable, Callable

from ..protocol import ErrorCode


class ProviderError(Exception):
    """Upstream failure mapped onto the device-facing error codes."""

    def __init__(self, code: ErrorCode, detail: str = "", retryable: bool | None = None):
        super().__init__(detail or code.value)
        self.code = code
        self.detail = detail
        self.retryable = retryable


@dataclass
class AsrResult:
    text: str
    is_final: bool
    begin_ms: int | None = None
    end_ms: int | None = None


@dataclass
class LlmUsage:
    input_tokens: int | None = None
    output_tokens: int | None = None


@dataclass
class AsrContract:
    """Declared upstream contract, logged at startup for the contract gate."""

    name: str
    endpoint: str
    auth: str
    region: str
    model: str
    input_format: str = "pcm_s16le, 16000 Hz, mono"
    chunk_ms: int = 100
    events: dict[str, str] = field(default_factory=dict)
    heartbeat: str = "provider-native"
    reconnect: str = "one retry, no resume"


class AsrProvider(ABC):
    """Push-based streaming ASR.

    Lifecycle: start() -> push_audio()* -> finish() -> close().
    Results are delivered through the callback registered with on_result().
    """

    contract: AsrContract

    def __init__(self) -> None:
        self._on_result: Callable[[AsrResult], Awaitable[None]] | None = None

    def on_result(self, cb: Callable[[AsrResult], Awaitable[None]]) -> None:
        self._on_result = cb

    async def _emit(self, result: AsrResult) -> None:
        if self._on_result is not None:
            await self._on_result(result)

    @abstractmethod
    async def start(self) -> None: ...

    @abstractmethod
    async def push_audio(self, pcm: bytes) -> None: ...

    @abstractmethod
    async def finish(self) -> None: ...

    @abstractmethod
    async def close(self) -> None: ...


class LlmProvider(ABC):
    """Streaming chat. on_delta receives text pieces in order."""

    contract: AsrContract  # same shape, reused for the startup contract log

    @abstractmethod
    async def stream(
        self,
        prompt: str,
        history: list[dict[str, str]],
        max_tokens: int,
        on_delta: Callable[[str], Awaitable[None]],
        system: str | None = None,
    ) -> tuple[str, LlmUsage]:
        """Return (full_text, usage). Partial text is pushed through on_delta.

        ``system`` overrides the configured system prompt for this call
        (used by /api/v1/chat mode="dict"); None keeps the default.

        The session owns first-token and total-timeout enforcement; providers
        report transport failures as ProviderError.
        """
        ...

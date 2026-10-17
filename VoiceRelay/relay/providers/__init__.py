"""Provider factory: pick adapters from configuration."""

from __future__ import annotations

from ..config import Settings
from ..protocol import ErrorCode
from .base import AsrProvider, LlmProvider, ProviderError


def make_asr_provider(settings: Settings) -> AsrProvider:
    if settings.asr_provider == "mock":
        from .mock import MockAsrProvider

        return MockAsrProvider(settings)
    if settings.asr_provider == "dashscope":
        from .asr_dashscope import DashScopeAsrProvider

        return DashScopeAsrProvider(settings)
    raise ProviderError(ErrorCode.INTERNAL_ERROR, f"unknown ASR_PROVIDER {settings.asr_provider!r}")


def make_llm_provider(settings: Settings) -> LlmProvider:
    if settings.llm_provider == "mock":
        from .mock import MockLlmProvider

        return MockLlmProvider(settings)
    if settings.llm_provider == "dashscope":
        from .llm_dashscope import DashScopeLlmProvider

        return DashScopeLlmProvider(settings)
    raise ProviderError(ErrorCode.INTERNAL_ERROR, f"unknown LLM_PROVIDER {settings.llm_provider!r}")


def contract_report(settings: Settings) -> list[dict]:
    """Declared upstream contracts, logged at startup (docs section 7)."""
    out: list[dict] = []
    for factory, kind in ((make_asr_provider, "asr"), (make_llm_provider, "llm")):
        try:
            provider = factory(settings)
        except ProviderError as exc:
            out.append({"kind": kind, "name": "unconfigured", "detail": str(exc)})
            continue
        contract = provider.contract
        out.append(
            {
                "kind": kind,
                "name": contract.name,
                "endpoint": contract.endpoint,
                "auth": contract.auth,
                "region": contract.region,
                "model": contract.model,
                "input_format": contract.input_format,
                "chunk_ms": contract.chunk_ms,
                "events": contract.events,
                "heartbeat": contract.heartbeat,
                "reconnect": contract.reconnect,
            }
        )
    return out

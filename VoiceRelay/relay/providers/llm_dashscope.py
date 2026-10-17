"""DashScope OpenAI-compatible streaming chat adapter (Qwen).

POST {base_url}/chat/completions with stream=true; SSE frames are
``data: {json}`` lines ending in ``data: [DONE]``. Handles: split SSE packets,
empty deltas, usage-only chunks (choices == []) and HTTP 401/403/429 mapping.
"""

from __future__ import annotations

import logging

import httpx
import orjson

from ..config import Settings
from ..protocol import ErrorCode
from .base import AsrContract, LlmProvider, LlmUsage, ProviderError

log = logging.getLogger("relay.llm.dashscope")

DASHSCOPE_LLM_CONTRACT = AsrContract(
    name="dashscope-openai-compatible",
    endpoint="https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions",
    auth="Bearer API key (region-scoped)",
    region="cn-beijing",
    model="qwen-flash",
    input_format="OpenAI chat messages, stream=true",
    chunk_ms=0,
    events={
        "llm.delta": "choices[0].delta.content non-empty",
        "llm.final": "data: [DONE] or accumulated text",
        "usage": "chunk.usage (may be usage-only chunk)",
    },
    heartbeat="SSE keep-alive comments (: ) ignored",
    reconnect="one retry on connect / 429, never on 401/403",
)


class DashScopeLlmProvider(LlmProvider):
    contract = DASHSCOPE_LLM_CONTRACT

    def __init__(self, settings: Settings):
        super().__init__()
        self.settings = settings
        self._client: httpx.AsyncClient | None = None

    def _get_client(self) -> httpx.AsyncClient:
        if self._client is None:
            self._client = httpx.AsyncClient(
                timeout=httpx.Timeout(
                    connect=self.settings.asr_connect_timeout,
                    read=self.settings.upstream_total_timeout,
                    write=10.0,
                    pool=5.0,
                ),
                limits=httpx.Limits(max_connections=4, max_keepalive_connections=2),
            )
        return self._client

    async def close(self) -> None:
        if self._client is not None:
            await self._client.aclose()
            self._client = None

    async def stream(
        self,
        prompt: str,
        history: list[dict[str, str]],
        max_tokens: int,
        on_delta,
        system: str | None = None,
    ) -> tuple[str, LlmUsage]:
        api_key = self.settings.dashscope_api_key
        if not api_key:
            raise ProviderError(ErrorCode.LLM_AUTH_ERROR, "DASHSCOPE_API_KEY not configured")
        url = f"{self.settings.dashscope_llm_base_url()}/chat/completions"
        system_prompt = system if system is not None else self.settings.llm_system_prompt
        messages: list[dict[str, str]] = [{"role": "system", "content": system_prompt}]
        messages.extend(history)
        messages.append({"role": "user", "content": prompt})
        body: dict = {
            "model": self.settings.llm_model,
            "messages": messages,
            "max_tokens": max_tokens,
            "stream": True,
            "stream_options": {"include_usage": True},
        }
        if self.settings.llm_disable_thinking:
            body["enable_thinking"] = False

        headers = {"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"}
        client = self._get_client()
        chunks: list[str] = []
        usage = LlmUsage()
        try:
            async with client.stream("POST", url, headers=headers, json=body) as resp:
                if resp.status_code in (401, 403):
                    raise ProviderError(ErrorCode.LLM_AUTH_ERROR, f"http {resp.status_code}")
                if resp.status_code == 429:
                    raise ProviderError(ErrorCode.LLM_RATE_LIMITED, "http 429")
                if resp.status_code >= 400:
                    detail = (await resp.aread())[:200]
                    raise ProviderError(
                        ErrorCode.LLM_CONNECTION_ERROR,
                        f"http {resp.status_code}: {detail!r}",
                    )
                async for line in resp.aiter_lines():
                    if not line or not line.startswith("data:"):
                        continue
                    data = line[5:].strip()
                    if data == "[DONE]":
                        break
                    try:
                        frame = orjson.loads(data)
                    except orjson.JSONDecodeError:
                        continue
                    if frame.get("usage"):
                        usage.input_tokens = frame["usage"].get("prompt_tokens", usage.input_tokens)
                        usage.output_tokens = frame["usage"].get("completion_tokens", usage.output_tokens)
                    choices = frame.get("choices") or []
                    if not choices:
                        continue  # usage-only chunk
                    delta = choices[0].get("delta") or {}
                    piece = delta.get("content")
                    if not piece:
                        continue  # role-only / empty delta
                    chunks.append(piece)
                    await on_delta(piece)
        except httpx.TimeoutException as exc:
            raise ProviderError(ErrorCode.LLM_CONNECTION_ERROR, f"timeout: {exc}") from exc
        except httpx.HTTPError as exc:
            raise ProviderError(ErrorCode.LLM_CONNECTION_ERROR, f"http error: {exc}") from exc
        text = "".join(chunks)
        if not text:
            raise ProviderError(ErrorCode.INTERNAL_ERROR, "empty completion")
        return text, usage
"""Relay configuration.

All secrets come from the environment (root-only EnvironmentFile on the
deployment host, see docs section 17.4). Nothing here is ever returned to a
device or written to a log.
"""

from __future__ import annotations

import os

from pydantic import Field, field_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=None, extra="ignore", case_sensitive=True)

    # --- process -----------------------------------------------------------
    relay_env: str = Field("development", alias="RELAY_ENV")
    relay_bind: str = Field("127.0.0.1", alias="RELAY_BIND")
    relay_port: int = Field(8000, alias="RELAY_PORT")
    relay_log_level: str = Field("INFO", alias="RELAY_LOG_LEVEL")
    # Release identifier surfaced by /healthz (set by the deploy script).
    relay_version: str = Field("0.1.0", alias="RELAY_VERSION")

    # --- device auth -------------------------------------------------------
    # Single shared device token (first version) OR a per-device map,
    # "deviceId:token,deviceId:token". A map entry wins over the shared token.
    relay_device_token: str = Field("", alias="RELAY_DEVICE_TOKEN")
    relay_device_tokens: str = Field("", alias="RELAY_DEVICE_TOKENS")
    # Empty list = any device id with a valid token. Production should pin the
    # real device id(s) so a leaked token cannot impersonate another device.
    allowed_device_ids: str = Field("", alias="ALLOWED_DEVICE_IDS")
    # Admin token for POST /api/v1/chat (text debug endpoint). Empty = the
    # endpoint is disabled (returns 404), per docs section 17.5.4.
    relay_admin_token: str = Field("", alias="RELAY_ADMIN_TOKEN")

    # --- providers ---------------------------------------------------------
    asr_provider: str = Field("mock", alias="ASR_PROVIDER")  # mock | dashscope
    asr_model: str = Field("paraformer-realtime-v2", alias="ASR_MODEL")
    asr_endpoint: str = Field("", alias="ASR_ENDPOINT")
    asr_language_hints: str = Field("zh,en", alias="ASR_LANGUAGE_HINTS")
    asr_chunk_ms: int = Field(100, alias="ASR_CHUNK_MS")

    llm_provider: str = Field("mock", alias="LLM_PROVIDER")  # mock | dashscope
    llm_model: str = Field("qwen-flash", alias="LLM_MODEL")
    llm_base_url: str = Field("", alias="LLM_BASE_URL")
    llm_system_prompt: str = Field(
        "你是一个阅读伴侣，回答简洁自然，面向朗读场景。",
        alias="LLM_SYSTEM_PROMPT",
    )
    # mode="dict" on /api/v1/chat: dictionary entries for the reader's
    # character-lookup card. The device renders 【...】 lines as sections.
    llm_dict_system_prompt: str = Field(
        "你是一本简明新华字典。用户发来一个汉字，你只输出这个字的条目，严格用下面格式"
        "（每项一行，不要 Markdown、不要解释、不要反问）：\n"
        "【字】<字>\n"
        "【拼音】<带声调的拼音；多音字按常用度列出，每个读音后跟一个词>\n"
        "【部首笔画】<部首>·<笔画数>画\n"
        "【释义】1. <释义> 2. <释义>\n"
        "【组词】<词1>、<词2>、<词3>\n"
        "【例句】<一句用到该字的话>\n"
        "内容务必准确精炼：释义最多 3 条，组词 3~5 个，全文不超过 300 字；"
        "若输入不是单个汉字，输出【字】无法识别。",
        alias="LLM_DICT_SYSTEM_PROMPT",
    )
    dict_max_tokens: int = Field(600, alias="LLM_DICT_MAX_TOKENS")
    llm_disable_thinking: bool = Field(True, alias="LLM_DISABLE_THINKING")

    dashscope_api_key: str = Field("", alias="DASHSCOPE_API_KEY")
    dashscope_workspace_id: str = Field("", alias="DASHSCOPE_WORKSPACE_ID")
    dashscope_region: str = Field("cn-beijing", alias="DASHSCOPE_REGION")

    openclaw_enabled: bool = Field(False, alias="OPENCLAW_ENABLED")
    openclaw_ws_url: str = Field("ws://127.0.0.1:18789/", alias="OPENCLAW_WS_URL")
    openclaw_token: str = Field("", alias="OPENCLAW_TOKEN")
    openclaw_keywords: str = Field("书库,下载,书架,工具,管理", alias="OPENCLAW_KEYWORDS")

    # --- limits / timeouts (docs section 8) --------------------------------
    max_audio_seconds: int = Field(30, alias="MAX_AUDIO_SECONDS")
    max_llm_output_tokens: int = Field(256, alias="MAX_LLM_OUTPUT_TOKENS")
    max_reply_chars: int = Field(500, alias="MAX_REPLY_CHARS")
    session_idle_timeout_seconds: int = Field(60, alias="SESSION_IDLE_TIMEOUT_SECONDS")
    asr_connect_timeout: float = Field(5.0, alias="ASR_CONNECT_TIMEOUT")
    asr_task_started_timeout: float = Field(3.0, alias="ASR_TASK_STARTED_TIMEOUT")
    asr_finish_timeout: float = Field(8.0, alias="ASR_FINISH_TIMEOUT")
    llm_first_token_timeout: float = Field(5.0, alias="LLM_FIRST_TOKEN_TIMEOUT")
    upstream_total_timeout: float = Field(30.0, alias="UPSTREAM_TOTAL_TIMEOUT")
    retry_backoff_seconds: float = Field(1.0, alias="RETRY_BACKOFF_SECONDS")
    max_concurrent_sessions: int = Field(4, alias="MAX_CONCURRENT_SESSIONS")
    # Multiple onepage devices share this relay: cap connections so a
    # reconnect loop cannot exhaust file descriptors, and cap sessions per
    # device so one device cannot occupy every ASR slot.
    max_ws_connections: int = Field(32, alias="MAX_WS_CONNECTIONS")
    max_sessions_per_device: int = Field(1, alias="MAX_SESSIONS_PER_DEVICE")

    # --- mock provider knobs (development) ---------------------------------
    mock_asr_text: str = Field("今天天气怎么样", alias="MOCK_ASR_TEXT")
    mock_asr_partials: str = Field("今天,今天天气", alias="MOCK_ASR_PARTIALS")
    mock_asr_partial_interval_ms: int = Field(500, alias="MOCK_ASR_PARTIAL_INTERVAL_MS")
    mock_asr_final_delay_ms: int = Field(150, alias="MOCK_ASR_FINAL_DELAY_MS")
    mock_llm_text: str = Field("今天适合阅读，室外天气晴朗。", alias="MOCK_LLM_TEXT")
    mock_llm_delta_interval_ms: int = Field(80, alias="MOCK_LLM_DELTA_INTERVAL_MS")
    mock_llm_delta_chars: int = Field(3, alias="MOCK_LLM_DELTA_CHARS")
    mock_asr_fail_start: bool = Field(False, alias="MOCK_ASR_FAIL_START")
    mock_llm_fail: bool = Field(False, alias="MOCK_LLM_FAIL")

    @field_validator("asr_chunk_ms")
    @classmethod
    def _chunk_bounds(cls, v: int) -> int:
        if v < 20 or v > 500:
            raise ValueError("ASR_CHUNK_MS must be 20..500")
        return v

    @field_validator("asr_provider")
    @classmethod
    def _asr_name(cls, v: str) -> str:
        # Docs section 17.4 uses ASR_PROVIDER=paraformer.
        return {"paraformer": "dashscope", "dashscope-paraformer": "dashscope"}.get(v.lower(), v.lower())

    @field_validator("llm_provider")
    @classmethod
    def _llm_name(cls, v: str) -> str:
        # Docs section 17.4 uses LLM_PROVIDER=dashscope-openai-compatible.
        return {"dashscope-openai-compatible": "dashscope", "openai-compatible": "dashscope"}.get(
            v.lower(), v.lower()
        )

    # ------------------------------------------------------------------ helpers
    @property
    def allowed_devices(self) -> list[str]:
        return [d.strip() for d in self.allowed_device_ids.split(",") if d.strip()]

    @property
    def language_hints(self) -> list[str]:
        return [h.strip() for h in self.asr_language_hints.split(",") if h.strip()]

    @property
    def openclaw_route_keywords(self) -> list[str]:
        return [k.strip() for k in self.openclaw_keywords.split(",") if k.strip()]

    def device_token_map(self) -> dict[str, str]:
        out: dict[str, str] = {}
        for pair in self.relay_device_tokens.split(","):
            if not pair.strip():
                continue
            device_id, _, token = pair.partition(":")
            if device_id.strip() and token.strip():
                out[device_id.strip()] = token.strip()
        return out

    def dashscope_asr_endpoint(self) -> str:
        if self.asr_endpoint:
            return self.asr_endpoint
        return "wss://dashscope.aliyuncs.com/api-ws/v1/inference"

    def dashscope_llm_base_url(self) -> str:
        if self.llm_base_url:
            return self.llm_base_url.rstrip("/")
        if self.dashscope_workspace_id:
            return (
                f"https://{self.dashscope_workspace_id}.{self.dashscope_region}"
                ".maas.aliyuncs.com/compatible-mode/v1"
            )
        return "https://dashscope.aliyuncs.com/compatible-mode/v1"

    def ready_report(self) -> dict[str, str]:
        """What /readyz reports. Never contains key material."""
        return {
            "asr": "configured" if self._asr_ready() else "missing_config",
            "llm": "configured" if self._llm_ready() else "missing_config",
            "region": self.dashscope_region,
        }

    def _asr_ready(self) -> bool:
        if self.asr_provider == "mock":
            return True
        return self.asr_provider == "dashscope" and bool(self.dashscope_api_key)

    def _llm_ready(self) -> bool:
        if self.llm_provider == "mock":
            return True
        return self.llm_provider == "dashscope" and bool(self.dashscope_api_key)


_settings: Settings | None = None


def get_settings() -> Settings:
    global _settings
    if _settings is None:
        _settings = Settings()
    return _settings


def reload_settings() -> Settings:
    """Test hook: re-read the environment."""
    global _settings
    _settings = Settings()
    return _settings

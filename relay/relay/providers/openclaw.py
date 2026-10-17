"""Optional OpenClaw routing adapter (docs phase 5: 需要时接入).

The relay keeps a single routing hook for tool/library tasks. The first
version ships the hook switched off (OPENCLAW_ENABLED=false) and routes every
question to the fast LLM; enabling it requires the OpenClaw gateway protocol
(client protocol v2, Ed25519 handshake) to be implemented against the local
gateway, and that must pass its own contract gate before production.
"""

from __future__ import annotations

import logging

from ..config import Settings
from ..protocol import ErrorCode
from .base import ProviderError

log = logging.getLogger("relay.openclaw")


def should_route_to_openclaw(settings: Settings, text: str) -> bool:
    if not settings.openclaw_enabled:
        return False
    return any(keyword and keyword in text for keyword in settings.openclaw_route_keywords)


async def answer_via_openclaw(settings: Settings, text: str) -> str:
    """Placeholder: raises until the adapter is implemented and contract-tested."""
    raise ProviderError(
        ErrorCode.INTERNAL_ERROR,
        "OpenClaw adapter not implemented; set OPENCLAW_ENABLED=false",
    )

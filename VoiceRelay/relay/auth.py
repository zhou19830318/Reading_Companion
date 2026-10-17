"""Device authentication.

Header contract (docs section 17.5):
    Authorization: Bearer <relay-device-token>
    X-Device-Id: <device-id>
    X-Voice-Protocol: 1

Tokens are compared in constant time. A per-device token map wins over the
shared token; an empty ALLOWED_DEVICE_IDS accepts any device id that holds a
valid token (development), a non-empty list pins the ids.
"""

from __future__ import annotations

import hmac

from .config import Settings
from .protocol import ErrorCode, ProtocolError


def _ct_eq(a: str, b: str) -> bool:
    return hmac.compare_digest(a.encode(), b.encode())


def check_device(settings: Settings, authorization: str | None, device_id: str | None) -> str:
    """Return the authenticated device id or raise ProtocolError."""
    token = ""
    if authorization:
        scheme, _, value = authorization.partition(" ")
        if scheme.lower() == "bearer":
            token = value.strip()
    if not token:
        raise ProtocolError(ErrorCode.UNAUTHORIZED, "missing bearer token")

    device_id = (device_id or "").strip()
    token_map = settings.device_token_map()
    if device_id and device_id in token_map:
        expected = token_map[device_id]
        if not _ct_eq(token, expected):
            raise ProtocolError(ErrorCode.UNAUTHORIZED, "token mismatch")
    elif settings.relay_device_token:
        if not _ct_eq(token, settings.relay_device_token):
            raise ProtocolError(ErrorCode.UNAUTHORIZED, "token mismatch")
    elif not token_map:
        # No tokens configured at all: refuse rather than run open.
        raise ProtocolError(ErrorCode.UNAUTHORIZED, "no device token configured")

    allowed = settings.allowed_devices
    if allowed and device_id not in allowed:
        raise ProtocolError(ErrorCode.DEVICE_NOT_ALLOWED, "device not allowed")
    return device_id


def check_admin(settings: Settings, authorization: str | None) -> None:
    token = ""
    if authorization:
        scheme, _, value = authorization.partition(" ")
        if scheme.lower() == "bearer":
            token = value.strip()
    if not settings.relay_admin_token or not token or not _ct_eq(token, settings.relay_admin_token):
        raise ProtocolError(ErrorCode.UNAUTHORIZED, "admin token required")

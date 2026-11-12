"""Device authentication.

Header contract (docs section 17.5):
    Authorization: Bearer <relay-device-token>
    X-Device-Id: <device-id>
    X-Voice-Protocol: 1

Tokens are compared in constant time. When a per-device token map is
configured, only ids in that map are accepted — the shared token is a
development fallback for deployments with no map, never an escape hatch for an
unknown id. An empty ALLOWED_DEVICE_IDS accepts any device id that holds a
valid token (development), a non-empty list pins the ids.
"""

from __future__ import annotations

import hmac
from typing import TYPE_CHECKING

from .config import Settings
from .protocol import ErrorCode, ProtocolError

if TYPE_CHECKING:
    from .device_store import DeviceStore


def _ct_eq(a: str, b: str) -> bool:
    return hmac.compare_digest(a.encode(), b.encode())


def check_device(
    settings: Settings,
    authorization: str | None,
    device_id: str | None,
    device_store: DeviceStore | None = None,
) -> str:
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
    managed_ids = set(device_store.ids()) if device_store is not None else set()
    if managed_ids:
        # The server registry is authoritative once it has devices. Unknown ids
        # must not borrow the environment's shared token.
        if not device_id or device_id not in managed_ids:
            raise ProtocolError(ErrorCode.UNAUTHORIZED, "unknown device id")
        candidates = device_store.auth_tokens(device_id)
        if not any(_ct_eq(token, candidate) for candidate in candidates):
            raise ProtocolError(ErrorCode.UNAUTHORIZED, "token mismatch")
    elif token_map:
        # A configured map is authoritative: an unknown id must not borrow the
        # shared token, and an empty id cannot be attributed to any device.
        if not device_id or device_id not in token_map:
            raise ProtocolError(ErrorCode.UNAUTHORIZED, "unknown device id")
        if not _ct_eq(token, token_map[device_id]):
            raise ProtocolError(ErrorCode.UNAUTHORIZED, "token mismatch")
    elif settings.relay_device_token:
        if not _ct_eq(token, settings.relay_device_token):
            raise ProtocolError(ErrorCode.UNAUTHORIZED, "token mismatch")
    else:
        # No tokens configured at all: refuse rather than run open.
        raise ProtocolError(ErrorCode.UNAUTHORIZED, "no device token configured")

    allowed = settings.allowed_devices
    if allowed and not managed_ids and device_id not in allowed:
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

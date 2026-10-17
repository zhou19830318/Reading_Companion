"""Device/admin auth unit tests."""

from __future__ import annotations

import pytest

from relay.auth import check_admin, check_device
from relay.config import Settings
from relay.protocol import ErrorCode, ProtocolError


def settings(**env) -> Settings:
    base = {
        "RELAY_DEVICE_TOKEN": "shared-token",
        "RELAY_ADMIN_TOKEN": "admin-token",
        "ALLOWED_DEVICE_IDS": "",  # neutralise the conftest environment
    }
    base.update(env)
    return Settings(**base)


def test_accepts_shared_token():
    assert check_device(settings(), "Bearer shared-token", "any-device") == "any-device"


def test_rejects_missing_or_malformed_header():
    for header in (None, "", "shared-token", "Basic shared-token", "Bearer "):
        with pytest.raises(ProtocolError) as exc:
            check_device(settings(), header, "d")
        assert exc.value.code == ErrorCode.UNAUTHORIZED


def test_rejects_wrong_token():
    with pytest.raises(ProtocolError):
        check_device(settings(), "Bearer nope", "d")


def test_refuses_when_no_token_configured():
    with pytest.raises(ProtocolError) as exc:
        check_device(Settings(RELAY_DEVICE_TOKEN="", RELAY_DEVICE_TOKENS="", ASR_PROVIDER="mock"), "Bearer x", "d")
    assert exc.value.code == ErrorCode.UNAUTHORIZED


def test_per_device_map_wins_over_shared_token():
    s = settings(RELAY_DEVICE_TOKENS="dev-a:token-a,dev-b:token-b")
    assert check_device(s, "Bearer token-a", "dev-a") == "dev-a"
    # shared token no longer works for a device that has its own entry
    with pytest.raises(ProtocolError) as exc:
        check_device(s, "Bearer shared-token", "dev-a")
    assert exc.value.code == ErrorCode.UNAUTHORIZED
    # unknown device falls back to the shared token
    assert check_device(s, "Bearer shared-token", "dev-c") == "dev-c"


def test_allowed_device_ids_pin_the_device():
    s = settings(ALLOWED_DEVICE_IDS="dev-a,dev-b")
    assert check_device(s, "Bearer shared-token", "dev-a") == "dev-a"
    with pytest.raises(ProtocolError) as exc:
        check_device(s, "Bearer shared-token", "dev-x")
    assert exc.value.code == ErrorCode.DEVICE_NOT_ALLOWED


def test_admin_token():
    check_admin(settings(), "Bearer admin-token")
    for header in (None, "", "Bearer wrong", "Bearer ", "nope"):
        with pytest.raises(ProtocolError):
            check_admin(settings(), header)
    with pytest.raises(ProtocolError):
        check_admin(settings(RELAY_ADMIN_TOKEN=""), "Bearer admin-token")

"""Server-side device registry.

The relay is the source of truth for each reader's relay URL and device token.
A device keeps its existing CloudConfig values as the bootstrap
credential; after that it can pull the current values from
``GET /api/v1/device/config`` (authenticated with its current token) and update
NVS only when something changed.

Secrets live in a mode-0600 JSON file under ``RELAY_DEVICE_DIR`` (default
``run/devices``). The file is intentionally separate from ``voice-relay.env``
so an operator can edit devices from /admin without restarting the service; the
environment remains the fallback for an unmanaged deployment.
"""

from __future__ import annotations

import json
import os
import re
import secrets
import threading
import time
from pathlib import Path
from typing import Any

from .config import Settings

DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
TOKEN_MIN = 16
TOKEN_MAX = 192
URL_MAX = 192
NAME_MAX = 64
PREVIOUS_TOKEN_TTL_S = 15 * 60


class DeviceStoreError(ValueError):
    """Invalid admin input; the caller turns this into HTTP 400."""


def _now() -> int:
    return int(time.time())


def _clean(value: Any, limit: int) -> str:
    if value is None:
        return ""
    return str(value).strip()[:limit]


def _record_int(value: Any) -> int:
    try:
        return max(0, int(value or 0))
    except (TypeError, ValueError):
        return 0


class DeviceStore:
    def __init__(self, path: Path) -> None:
        self.path = path
        self._lock = threading.RLock()
        self._devices: dict[str, dict[str, Any]] | None = None

    # ---- persistence -----------------------------------------------------

    def _load(self) -> dict[str, dict[str, Any]]:
        if self._devices is not None:
            return self._devices
        devices: dict[str, dict[str, Any]] = {}
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            data = {}
        except (OSError, ValueError):
            # A corrupt registry must not prevent the relay from starting with
            # its environment fallback. Keep the bad file for inspection.
            data = {}
        raw = data.get("devices") if isinstance(data, dict) else None
        if isinstance(raw, dict):
            for device_id, record in raw.items():
                if not isinstance(record, dict) or not DEVICE_ID_RE.match(str(device_id)):
                    continue
                normalized = self._normalize_record(str(device_id), record)
                if normalized["token"]:
                    devices[str(device_id)] = normalized
        self._devices = devices
        return devices

    def _save_locked(self, devices: dict[str, dict[str, Any]]) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.chmod(self.path.parent, 0o700)
        except OSError:
            pass
        temporary = self.path.with_suffix(self.path.suffix + ".tmp")
        payload = {"version": 1, "devices": devices}
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as stream:
                json.dump(payload, stream, ensure_ascii=False, indent=2, sort_keys=True)
                stream.write("\n")
            os.replace(temporary, self.path)
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass

    @staticmethod
    def _normalize_record(device_id: str, record: dict[str, Any]) -> dict[str, Any]:
        return {
            "deviceId": device_id,
            "name": _clean(record.get("name"), NAME_MAX),
            "token": _clean(record.get("token"), TOKEN_MAX),
            "relayUrl": _clean(record.get("relayUrl"), URL_MAX),
            "createdAt": _record_int(record.get("createdAt")),
            "updatedAt": _record_int(record.get("updatedAt")),
            "previousToken": _clean(record.get("previousToken"), TOKEN_MAX),
            "previousTokenExpiresAt": _record_int(record.get("previousTokenExpiresAt")),
        }

    # ---- reads -----------------------------------------------------------

    def list(self) -> list[dict[str, Any]]:
        with self._lock:
            devices = self._load()
            return [
                {
                    "deviceId": record["deviceId"],
                    "name": record["name"],
                    "relayUrl": record["relayUrl"],
                    "deviceToken": record["token"],
                    "createdAt": record["createdAt"],
                    "updatedAt": record["updatedAt"],
                }
                for record in sorted(devices.values(), key=lambda row: row["deviceId"].lower())
            ]

    def get(self, device_id: str) -> dict[str, Any] | None:
        with self._lock:
            record = self._load().get(device_id)
            return dict(record) if record else None

    def ids(self) -> list[str]:
        with self._lock:
            return list(self._load().keys())

    def auth_tokens(self, device_id: str) -> list[str]:
        """Current token plus a short-lived previous token after a rotation."""
        with self._lock:
            record = self._load().get(device_id)
            if not record:
                return []
            tokens = [record["token"]]
            if record["previousToken"] and record["previousTokenExpiresAt"] >= _now():
                tokens.append(record["previousToken"])
            return [token for token in tokens if token]

    def token_map(self) -> dict[str, str]:
        with self._lock:
            return {device_id: record["token"] for device_id, record in self._load().items()}

    def config_for(self, device_id: str) -> dict[str, Any] | None:
        """Device config consumed by the admin UI and config endpoint."""
        with self._lock:
            record = self._load().get(device_id)
            if not record:
                return None
            return {
                "deviceId": record["deviceId"],
                "name": record["name"],
                "relayUrl": record["relayUrl"],
                "token": record["token"],
                "updatedAt": record["updatedAt"],
            }

    # ---- writes ----------------------------------------------------------

    def upsert(
        self,
        device_id: str,
        *,
        name: str | None = None,
        relay_url: str | None = None,
        token: str | None = None,
    ) -> dict[str, Any]:
        device_id = str(device_id or "").strip()
        if len(device_id) > 64 or not DEVICE_ID_RE.match(device_id):
            raise DeviceStoreError("deviceId must be 1..64 chars: letters, digits, . _ -")

        with self._lock:
            devices = self._load()
            existing = devices.get(device_id)
            now = _now()
            record = dict(existing) if existing else {
                "deviceId": device_id,
                "name": "",
                "token": "",
                "relayUrl": "",
                "createdAt": now,
                "updatedAt": now,
                "previousToken": "",
                "previousTokenExpiresAt": 0,
            }

            if name is not None:
                record["name"] = _clean(name, NAME_MAX)
            if relay_url is not None:
                relay_url = str(relay_url).strip()
                if len(relay_url) > URL_MAX:
                    raise DeviceStoreError(f"relayUrl must be at most {URL_MAX} characters")
                if relay_url and not relay_url.startswith(("ws://", "wss://")):
                    raise DeviceStoreError("relayUrl must start with ws:// or wss://")
                record["relayUrl"] = relay_url
            if token is not None:
                token = str(token).strip()
                if not token:
                    raise DeviceStoreError("deviceToken must not be empty")
                if len(token) < TOKEN_MIN:
                    raise DeviceStoreError(f"deviceToken must be at least {TOKEN_MIN} characters")
                if len(token) > TOKEN_MAX:
                    raise DeviceStoreError(f"deviceToken must be at most {TOKEN_MAX} characters")
                if token != record["token"]:
                    if record["token"]:
                        record["previousToken"] = record["token"]
                        record["previousTokenExpiresAt"] = now + PREVIOUS_TOKEN_TTL_S
                    record["token"] = token
            elif not record["token"]:
                record["token"] = secrets.token_urlsafe(24)

            record["updatedAt"] = now
            devices[device_id] = record
            self._save_locked(devices)
            return self.config_for(device_id) or {}

    def delete(self, device_id: str) -> bool:
        with self._lock:
            devices = self._load()
            if device_id not in devices:
                return False
            del devices[device_id]
            self._save_locked(devices)
            return True


def make_store(settings: Settings) -> DeviceStore:
    return DeviceStore(settings.device_db_path)

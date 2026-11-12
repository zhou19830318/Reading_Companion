"""WeRead Skill API bridge.

The API key is a user-scoped, read-only ``wrk-`` credential. It is configured
from the relay admin web page and lives either in a mode-0600 file under the
relay run directory or in ``WEREAD_API_KEY``. It is never returned to a device,
never written to a log, and never stored in the reader's NVS/SD card.

Only the official gateway is used. There is no Web Reader scraping, no content
download, and no progress write-back: the official Skill API exposes none of
those operations.
"""

from __future__ import annotations

import asyncio
import logging
import os
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

import httpx

from .config import Settings

if TYPE_CHECKING:
    from .device_store import DeviceStore

log = logging.getLogger("relay.weread")

GATEWAY_URL = "https://i.weread.qq.com/api/agent/gateway"
VALID_STATS_MODES = {"weekly", "monthly", "annually", "overall"}
MAX_SHELF_LIMIT = 50


class WeReadError(Exception):
    """An upstream or configuration failure with a device-safe error token."""

    def __init__(self, code: str, status_code: int = 502, detail: str = "") -> None:
        super().__init__(detail or code)
        self.code = code
        self.status_code = status_code
        self.detail = detail


def _int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _str(value: Any) -> str:
    if isinstance(value, str):
        return value
    return ""


def _epoch(value: Any) -> int:
    return max(0, _int(value, 0))


class WeReadStore:
    def __init__(self, settings: Settings, device_store: DeviceStore | None = None) -> None:
        self.settings = settings
        self.device_store = device_store
        base = Path(settings.relay_weread_dir or "run/weread")
        self.directory = base
        self.key_path = Path(settings.weread_key_file) if settings.weread_key_file else base / "api_key"
        self._file_key: str | None = None
        self._file_key_loaded = False
        self._active_key_path: Path | None = None
        # One cache per device: two readers may use different WeRead accounts.
        self._shelf_cache: dict[str, tuple[float, dict[str, Any]]] = {}
        self._shelf_lock = asyncio.Lock()
        self._progress_cache: dict[tuple[str, str], tuple[float, dict[str, Any]]] = {}
        self._stats_cache: dict[tuple[str, str], tuple[float, dict[str, Any]]] = {}

    # ---- key storage -----------------------------------------------------

    def _project_key_candidates(self) -> list[Path]:
        # The operator may drop the key in the project root as
        # "微信读书key". Tests must not accidentally read a developer's real
        # credential, so this fallback is disabled under RELAY_ENV=test.
        if self.settings.relay_env == "test":
            return []
        roots = [Path.cwd(), Path.cwd().parent, Path(__file__).resolve().parents[2]]
        return [root / "微信读书key" for root in roots]

    def _read_file_key(self) -> str:
        if self._file_key_loaded:
            return self._file_key or ""
        self._file_key_loaded = True
        candidates = [self.key_path, *self._project_key_candidates()]
        for candidate in candidates:
            try:
                if candidate.exists():
                    value = candidate.read_text(encoding="utf-8").strip()
                    if value:
                        self._file_key = value
                        self._active_key_path = candidate
                        if candidate != self.key_path:
                            log.info("weread key loaded from project file: %s", candidate)
                        return value
            except OSError as exc:
                log.warning("weread key file read failed: %s", exc)
        return ""

    def _device_key(self, device_id: str) -> str:
        if self.device_store and device_id:
            return self.device_store.weread_key(device_id)
        return ""

    def api_key(self, device_id: str = "") -> str:
        # A per-device key wins over the legacy global file. The file remains
        # the fallback for single-account deployments and the project-root
        # "微信读书key" bootstrap.
        return self._device_key(device_id) or self._read_file_key() or self.settings.weread_api_key.strip()

    @property
    def configured(self) -> bool:
        return bool(self.api_key())

    def configured_for(self, device_id: str = "") -> bool:
        return bool(self.api_key(device_id))

    def source(self, device_id: str = "") -> str:
        if self._device_key(device_id):
            return "device"
        if self._read_file_key():
            return "file" if self._active_key_path == self.key_path else "project_file"
        if self.settings.weread_api_key.strip():
            return "env"
        return "none"

    def key_updated_at(self, device_id: str = "") -> int:
        if self._device_key(device_id):
            return self.device_store.weread_updated_at(device_id) if self.device_store else 0
        try:
            return int((self._active_key_path or self.key_path).stat().st_mtime)
        except OSError:
            return 0

    def status(self, device_id: str = "") -> dict[str, Any]:
        cached = self._shelf_cache.get(device_id)
        return {
            "ok": True,
            "deviceId": device_id,
            "configured": self.configured_for(device_id),
            "source": self.source(device_id),
            "skillVersion": self.settings.weread_skill_version,
            "keyUpdatedAt": self.key_updated_at(device_id),
            "shelfCachedAt": int(cached[0]) if cached else 0,
        }

    def set_api_key(self, key: str, device_id: str = "") -> None:
        key = key.strip()
        if not key.startswith("wrk-") or not (20 <= len(key) <= 512):
            raise ValueError("api key must start with wrk- and be 20..512 characters")
        if device_id:
            if self.device_store is None or self.device_store.get(device_id) is None:
                raise ValueError("unknown deviceId")
            self.device_store.upsert(device_id, weread_key=key)
            self._clear_cache(device_id)
            log.info("weread api key saved for device %s", device_id)
            return
        self.key_path.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.chmod(self.key_path.parent, 0o700)
        except OSError:
            pass
        temporary = self.key_path.with_suffix(self.key_path.suffix + ".tmp")
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as stream:
                stream.write(key + "\n")
            os.replace(temporary, self.key_path)
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
        self._file_key = key
        self._file_key_loaded = True
        self._active_key_path = self.key_path
        self._clear_cache()
        log.info("weread api key saved from admin page")

    def clear_api_key(self, device_id: str = "") -> None:
        if device_id:
            if self.device_store is None or self.device_store.get(device_id) is None:
                raise ValueError("unknown deviceId")
            self.device_store.upsert(device_id, clear_weread=True)
            self._clear_cache(device_id)
            log.info("weread api key cleared for device %s", device_id)
            return
        try:
            self.key_path.unlink(missing_ok=True)
        except OSError as exc:
            raise ValueError(f"could not remove key file: {exc}") from exc
        self._file_key = ""
        # Re-discover a project-root fallback if one exists; the relay-owned
        # api_key file was the one just removed.
        self._file_key_loaded = False
        self._active_key_path = None
        self._clear_cache()
        log.info("weread api key file cleared")

    def invalidate(self, device_id: str = "") -> None:
        self._clear_cache(device_id)

    def _clear_cache(self, device_id: str = "") -> None:
        if device_id:
            self._shelf_cache.pop(device_id, None)
            for key in [key for key in self._progress_cache if key[0] == device_id]:
                self._progress_cache.pop(key, None)
            for key in [key for key in self._stats_cache if key[0] == device_id]:
                self._stats_cache.pop(key, None)
            return
        self._shelf_cache.clear()
        self._progress_cache.clear()
        self._stats_cache.clear()

    # ---- official gateway ------------------------------------------------

    async def _gateway(
        self, api_name: str, params: dict[str, Any] | None = None, device_id: str = ""
    ) -> dict[str, Any]:
        key = self.api_key(device_id)
        if not key:
            raise WeReadError("not_configured", 409, "WEREAD_API_KEY is not configured")

        payload: dict[str, Any] = {
            "api_name": api_name,
            "skill_version": self.settings.weread_skill_version,
        }
        if params:
            payload.update(params)

        timeout = httpx.Timeout(self.settings.weread_timeout_s)
        try:
            async with httpx.AsyncClient(timeout=timeout) as client:
                response = await client.post(
                    GATEWAY_URL,
                    json=payload,
                    headers={
                        "Authorization": f"Bearer {key}",
                        "Accept": "application/json",
                        "Content-Type": "application/json",
                        "User-Agent": "ReadingCompanionRelay/1.0",
                    },
                )
        except httpx.TimeoutException as exc:
            raise WeReadError("upstream_timeout", 504, str(exc)) from exc
        except httpx.HTTPError as exc:
            raise WeReadError("upstream", 502, str(exc)) from exc

        if response.status_code in (401, 403):
            raise WeReadError("weread_auth", 502, f"gateway HTTP {response.status_code}")
        if response.status_code == 429:
            raise WeReadError("rate_limited", 429, "gateway rate limited")
        if response.status_code >= 500:
            raise WeReadError("upstream", 502, f"gateway HTTP {response.status_code}")

        try:
            data = response.json()
        except ValueError as exc:
            raise WeReadError("upstream", 502, "gateway returned non-JSON") from exc
        if not isinstance(data, dict):
            raise WeReadError("upstream", 502, "gateway returned non-object JSON")
        if response.status_code >= 400:
            raise WeReadError("upstream", 502, f"gateway HTTP {response.status_code}")

        errcode = _int(data.get("errcode"), 0)
        if errcode:
            if errcode in (-2013, -2010, -2012):
                raise WeReadError("weread_auth", 502, f"gateway errcode {errcode}")
            raise WeReadError("upstream", 502, f"gateway errcode {errcode}")

        upgrade = data.get("upgrade_info")
        if upgrade:
            # The official Skill contract can ask the caller to upgrade. Keep
            # serving this request, but make the condition visible to operators.
            log.warning("weread skill reported upgrade_info: %.200s", str(upgrade))
        return data

    # ---- cached views ----------------------------------------------------

    async def _shelf_raw(self, force: bool = False, device_id: str = "") -> dict[str, Any]:
        now = time.time()
        ttl = self.settings.weread_shelf_ttl_s
        cached = self._shelf_cache.get(device_id)
        if not force and cached and now - cached[0] < ttl:
            return cached[1]
        async with self._shelf_lock:
            now = time.time()
            cached = self._shelf_cache.get(device_id)
            if not force and cached and now - cached[0] < ttl:
                return cached[1]
            raw = await self._gateway("/shelf/sync", {}, device_id=device_id)
            self._shelf_cache[device_id] = (time.time(), raw)
            return raw

    @staticmethod
    def _compact_book(item: dict[str, Any]) -> dict[str, Any]:
        book_id = _str(item.get("bookId")).strip()
        return {
            "bookId": book_id,
            "title": _str(item.get("title")),
            "author": _str(item.get("author")),
            "finished": bool(_int(item.get("finishReading"), 0)),
            "lastRead": _epoch(item.get("readUpdateTime")),
            "top": bool(_int(item.get("isTop"), 0)),
        }

    @staticmethod
    def _reader_link(item: dict[str, Any]) -> str:
        return _str(item.get("readerLink")) or _str(item.get("deepLink"))

    async def shelf(
        self, limit: int = 20, offset: int = 0, force: bool = False, device_id: str = ""
    ) -> dict[str, Any]:
        limit = max(1, min(MAX_SHELF_LIMIT, limit))
        offset = max(0, offset)
        raw = await self._shelf_raw(force=force, device_id=device_id)
        books_raw = raw.get("books")
        if not isinstance(books_raw, list):
            books_raw = []

        books: list[dict[str, Any]] = []
        for item in books_raw:
            if not isinstance(item, dict):
                continue
            compact = self._compact_book(item)
            if not compact["bookId"]:
                continue
            books.append(compact)

        # Pinned books first, then most recently read. The official order is
        # not guaranteed and includes user-pinned entries.
        books.sort(key=lambda entry: (entry["top"], entry["lastRead"]), reverse=True)
        total = len(books)
        return {
            "ok": True,
            "total": total,
            "offset": offset,
            "limit": limit,
            "books": books[offset : offset + limit],
            "cachedAt": int(self._shelf_cache[device_id][0]) if device_id in self._shelf_cache else 0,
        }

    async def book(self, book_id: str, device_id: str = "") -> dict[str, Any]:
        book_id = book_id.strip()
        if not book_id:
            raise WeReadError("book_not_found", 404, "bookId is empty")

        raw = await self._shelf_raw(device_id=device_id)
        shelf_book: dict[str, Any] | None = None
        for item in raw.get("books") or []:
            if isinstance(item, dict) and _str(item.get("bookId")).strip() == book_id:
                shelf_book = item
                break
        if shelf_book is None:
            raise WeReadError("book_not_found", 404, f"bookId {book_id} not on shelf")

        progress_raw = await self._progress(book_id, device_id=device_id)
        progress_book = progress_raw.get("book")
        if not isinstance(progress_book, dict):
            progress_book = progress_raw

        return {
            "ok": True,
            "bookId": book_id,
            "title": _str(shelf_book.get("title")),
            "author": _str(shelf_book.get("author")),
            "finished": bool(_int(shelf_book.get("finishReading"), 0)),
            "progress": max(0, min(100, _int(progress_book.get("progress"), 0))),
            "readingTime": _epoch(progress_book.get("recordReadingTime")),
            "finishTime": _epoch(progress_book.get("finishTime")),
            "chapterUid": _int(progress_book.get("chapterUid"), 0),
            "chapterOffset": _int(progress_book.get("chapterOffset"), 0),
            "readerLink": self._reader_link(shelf_book),
            "cachedAt": int(time.time()),
        }

    async def _progress(self, book_id: str, device_id: str = "") -> dict[str, Any]:
        cache_key = (device_id, book_id)
        now = time.time()
        cached = self._progress_cache.get(cache_key)
        if cached and now - cached[0] < self.settings.weread_progress_ttl_s:
            return cached[1]
        raw = await self._gateway("/book/getprogress", {"bookId": book_id}, device_id=device_id)
        self._progress_cache[cache_key] = (time.time(), raw)
        return raw

    async def stats(self, mode: str = "overall", device_id: str = "") -> dict[str, Any]:
        mode = mode if mode in VALID_STATS_MODES else "overall"
        cache_key = (device_id, mode)
        now = time.time()
        cached = self._stats_cache.get(cache_key)
        if cached and now - cached[0] < self.settings.weread_stats_ttl_s:
            return cached[1]

        params: dict[str, Any] = {"mode": mode}
        if mode != "overall":
            params["baseTime"] = int(now)
        raw = await self._gateway("/readdata/detail", params, device_id=device_id)
        self._stats_cache[cache_key] = (time.time(), raw)
        return {
            "ok": True,
            "mode": mode,
            "totalReadTime": _epoch(raw.get("totalReadTime")),
            "readDays": _epoch(raw.get("readDays")),
            "dayAverageReadTime": _epoch(raw.get("dayAverageReadTime")),
            "cachedAt": int(time.time()),
        }

"""Bounded, read-only market snapshots. No trading or generated prices."""

from __future__ import annotations

import asyncio
import json
import logging
import math
import re
import time
from collections import OrderedDict, deque
from dataclasses import dataclass, replace
from datetime import datetime, timedelta
from urllib.parse import quote
from zoneinfo import ZoneInfo

import httpx

from .config import Settings

log = logging.getLogger("relay.markets")


@dataclass(frozen=True)
class Instrument:
    id: str
    symbol: str
    group: str
    name: str
    currency: str = "CNY"
    unit: str = "index"
    timezone: str = "Asia/Shanghai"
    source: str = "tencent"


# Pinned board entries. User symbols are separately validated by resolve().
INSTRUMENTS = (
    Instrument("sse", "sh000001", "cn", "SSE Composite"),
    Instrument("szse", "sz399001", "cn", "SZSE Component"),
    Instrument("csi300", "sh000300", "cn", "CSI 300"),
    Instrument("chinext", "sz399006", "cn", "ChiNext"),
    Instrument("hsi", "hkHSI", "hk", "Hang Seng", "HKD", timezone="Asia/Hong_Kong"),
    Instrument("hscei", "hkHSCEI", "hk", "HSCEI", "HKD", timezone="Asia/Hong_Kong"),
    Instrument("sp500", "^GSPC", "us", "S&P 500", "USD", timezone="America/New_York", source="yahoo"),
    Instrument("nasdaq", "^IXIC", "us", "NASDAQ Composite", "USD", timezone="America/New_York", source="yahoo"),
    Instrument("dow", "^DJI", "us", "Dow Jones", "USD", timezone="America/New_York", source="yahoo"),
    Instrument("gold", "GC=F", "commodity", "Gold futures", "USD", "oz", "America/Chicago", "yahoo"),
    Instrument("silver", "SI=F", "commodity", "Silver futures", "USD", "oz", "America/Chicago", "yahoo"),
    Instrument("wti", "CL=F", "commodity", "WTI futures", "USD", "bbl", "America/New_York", "yahoo"),
    Instrument("brent", "BZ=F", "commodity", "Brent futures", "USD", "bbl", "America/New_York", "yahoo"),
)
BY_ID = {item.id: item for item in INSTRUMENTS}
GROUPS = ("cn", "hk", "us", "commodity")
PERIODS = {"1d": "5m", "1mo": "1d", "1y": "1d"}
# Yahoo is the primary feed for US/commodities, but it is not reachable from
# every network (mainland China in particular). These are quote-compatible
# fallbacks for the same instruments, used only when Yahoo fails.
TENCENT_US = {"^GSPC": "usINX", "^IXIC": "usIXIC", "^DJI": "usDJI"}
SINA_FUTURES = {"gold": "GC", "silver": "SI", "wti": "CL", "brent": "OIL"}
# Eastmoney is reachable from mainland China and serves the last US session
# around the clock, unlike Tencent's minute feed which is empty before the open.
EASTMONEY_US = {"sp500": "100.SPX", "nasdaq": "100.NDX", "dow": "100.DJIA"}
# CNBC's chart feed carries the last two US sessions as 1-minute bars. It is
# gzip (~17 KB on the wire for an index) and answers from Akamai, which the
# relay's mainland cloud host can reach; Eastmoney's edge drops that same host
# without a response, and Yahoo is blocked outright. Kept for five minutes:
# the US line moves slowly and the cross-border route is the slowest link.
CNBC_US = {"sp500": ".SPX", "nasdaq": ".IXIC", "dow": ".DJI"}
CNBC_CHART = "https://ts-api.cnbc.com/harmony/app/charts/1D.json"
CNBC_TTL_S = 300.0
# The cross-border route is slow in bursts, but gzip keeps an index under
# 20 KB; this only has to outlast a delay, since the whole request must still
# fit the device's 18 s wait together with any fallback.
CNBC_TIMEOUT_S = 6.0
# Minimum spacing between Eastmoney calls (seconds).
EASTMONEY_GAP_S = 1.2
# Eastmoney's edge drops the connection without a response when the User-Agent
# looks scripted ("Mozilla/5.0" alone is enough to be rejected); Yahoo's edge
# answers bare agents with its HTML block page. Identify as a real browser.
BROWSER_HEADERS = {
    "User-Agent": ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                   "(KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36"),
    "Accept": "*/*",
    "Accept-Language": "zh-CN,zh;q=0.9,en;q=0.8",
}
MAX_POINTS = 96
MAX_UPSTREAM_BYTES = 512 * 1024
# Sina returns full US daily history for stock lookups (IBM is ~1.2 MB).
MAX_DAILY_BYTES = 4 * 1024 * 1024
MAX_CACHE_KEYS = 128


class MarketError(Exception):
    def __init__(self, code: str, status: int = 502):
        super().__init__(code)
        self.code, self.status = code, status


# Upstreams return assorted malformed payloads; any of these means "try the
# next source" rather than a 500.
UPSTREAM_ERRORS = (MarketError, httpx.HTTPError, httpx.InvalidURL, TimeoutError, ValueError, KeyError,
                   TypeError, IndexError, AttributeError, OverflowError, StopIteration)


def resolve(code: str, allow_indexes: bool = True) -> Instrument:
    if allow_indexes and code in BY_ID:
        return BY_ID[code]
    code = code.strip().upper()
    if len(code) > 16:
        raise MarketError("invalid_stock_code", 400)
    # Do not guess an exchange for six-digit codes outside the supported
    # Shanghai/Shenzhen stock prefixes (e.g. Beijing shares).
    if re.fullmatch(r"[036]\d{5}", code):
        code = ("SH" if code[0] == "6" else "SZ") + code
    if re.fullmatch(r"\d{4,5}", code):
        code = "HK" + code.zfill(5)
    match = re.fullmatch(r"(SH|SZ)(\d{6})", code)
    if match:
        symbol = code.lower()
        return Instrument(code, symbol, "cn", code, unit="share")
    match = re.fullmatch(r"HK(\d{4,5})", code)
    if match:
        code = "HK" + match[1].zfill(5)
        return Instrument(code, code.lower(), "hk", code, "HKD", "share", "Asia/Hong_Kong")
    ticker = code.removeprefix("US.")
    if re.fullmatch(r"[A-Z]{1,6}([.-][A-Z])?", ticker):
        # Yahoo uses BRK-B for class shares, while users commonly type BRK.B.
        ticker = ticker.replace(".", "-")
        return Instrument("US." + ticker, ticker, "us", ticker, "USD", "share",
                          "America/New_York", "yahoo")
    raise MarketError("invalid_stock_code", 400)


def number(value):
    if isinstance(value, bool):
        return None
    try:
        result = float(value)
        return result if math.isfinite(result) else None
    except (ValueError, TypeError, OverflowError):
        return None


def compact(points: list, limit: int) -> list:
    """Keep endpoints and per-bucket extrema, in time order, without smoothing."""
    if len(points) <= limit:
        return points
    buckets = (limit - 2) // 2
    result = [points[0]]
    for i in range(buckets):
        chunk = points[1 + i * (len(points) - 2) // buckets:1 + (i + 1) * (len(points) - 2) // buckets]
        low, high = min(chunk, key=lambda p: p[1]), max(chunk, key=lambda p: p[1])
        result.extend(sorted({tuple(low), tuple(high)}))
    result.append(points[-1])
    return [list(p) for p in result]


def metadata(item: Instrument) -> dict:
    return {
        "id": item.id, "name": item.name, "symbol": item.symbol, "group": item.group,
        "currency": item.currency, "unit": item.unit, "timezone": item.timezone,
        "source": item.source,
        "kind": "stock" if item.unit == "share" else ("index" if item.unit == "index" else "futures"),
        # Public feeds have no guaranteed latency; never label them "live".
        "delayed": True,
    }


def stamp_zone(text: str, zone: str) -> int:
    digits = "".join(c for c in text if c.isdigit())
    fmt = "%Y%m%d%H%M%S" if len(digits) == 14 else "%Y%m%d%H%M"
    return int(datetime.strptime(digits, fmt).replace(tzinfo=ZoneInfo(zone)).timestamp())


def stamp(text: str, item: Instrument) -> int:
    return stamp_zone(text, item.timezone)


def jsonp(body: bytes):
    """Extract the JSON payload from a Sina jsonp/var wrapper."""
    text = body.decode("utf-8", "replace")
    start, end = text.find("("), text.rfind(")")
    if start < 0 or end <= start:
        raise ValueError("invalid jsonp")
    return json.loads(text[start + 1:end])


def last_session(points: list, item: Instrument) -> list:
    """Trim intraday points to the newest session with at least two prints.

    Yahoo's range=1d window is the current calendar day in the exchange zone;
    before the open it holds a single point, so the last real session is used
    instead of dropping the chart.
    """
    zone = ZoneInfo(item.timezone)
    sessions: dict = OrderedDict()
    for ts, value in points:
        stamp_value = number(ts)
        if stamp_value is None or not 0 < stamp_value < 4102444800:
            continue
        sessions.setdefault(datetime.fromtimestamp(stamp_value, zone).date(), []).append((ts, value))
    for day in reversed(sessions):
        if sum(number(value) is not None for _, value in sessions[day]) >= 2:
            return sessions[day]
    return points


def snapshot(item: Instrument, period: str, price, previous, as_of, points: list, high=None, low=None) -> dict:
    price, previous, as_of = number(price), number(previous), number(as_of)
    if price is None or as_of is None or not 0 < as_of < 4102444800:
        raise MarketError("invalid_upstream")
    clean = {}
    for ts, value in points:
        ts, value = number(ts), number(value)
        if ts is not None and 0 < ts < 4102444800 and value is not None:
            clean[int(ts)] = value
    ordered = [[ts, value] for ts, value in sorted(clean.items())]
    change = price - previous if previous is not None else None
    percent = change / previous * 100 if change is not None and previous and previous > 0 else None
    zone = ZoneInfo(item.timezone)
    label_fmt = "%m-%d %H:%M" if period == "1d" else "%Y-%m-%d"
    return {
        **metadata(item), "available": True, "period": period, "price": price,
        "previousClose": previous, "change": change, "changePct": percent,
        "high": number(high), "low": number(low), "asOf": int(as_of),
        "timeLabel": datetime.fromtimestamp(as_of, zone).strftime("%m-%d %H:%M"),
        "points": compact(ordered, MAX_POINTS), "chartAvailable": len(ordered) >= 2,
        "startLabel": datetime.fromtimestamp(ordered[0][0], zone).strftime(label_fmt) if ordered else "",
        "endLabel": datetime.fromtimestamp(ordered[-1][0], zone).strftime(label_fmt) if ordered else "",
    }


class MarketStore:
    def __init__(self, settings: Settings):
        self.settings = settings
        self.cache = OrderedDict()
        self.retry_after = OrderedDict()
        # Fixed lock stripes also bound memory for arbitrary valid stock codes.
        self.locks = [asyncio.Lock() for _ in range(32)]
        self.misses = deque()
        self.semaphore = asyncio.Semaphore(4)
        # Eastmoney resets a burst of requests from one IP (the US board asks
        # for three instruments at once), so its calls are spaced and the
        # window re-armed only after the previous call was issued.
        self.eastmoney_gate = asyncio.Lock()
        self.eastmoney_next = 0.0
        # Parsed CNBC sessions per instrument, so the 60 s board TTL does not
        # repay the cross-border download every minute.
        self.cnbc_cache: dict[str, tuple[float, dict]] = {}

    async def _text(self, url: str, params: dict | None = None, headers: dict | None = None,
                    max_bytes: int = MAX_UPSTREAM_BYTES, timeout: float | None = None) -> bytes:
        request_headers = {**BROWSER_HEADERS, **(headers or {})}
        try:
            # trust_env=False keeps an ambient proxy setting (and httpx's
            # strict no_proxy parsing, which rejects values like [::1]) from
            # breaking every upstream call.
            client = httpx.AsyncClient(timeout=timeout or self.settings.market_timeout_s,
                                       follow_redirects=False, trust_env=False)
        except httpx.InvalidURL as exc:
            # A malformed proxy entry in the service environment must not turn
            # into a 500 for every market request.
            raise MarketError("upstream_unavailable") from exc
        async with client:
            for attempt in range(2):
                try:
                    async with client.stream("GET", url, params=params or {},
                                             headers=request_headers) as response:
                        if response.status_code == 429:
                            raise MarketError("rate_limited")
                        if response.status_code != 200:
                            raise MarketError("upstream_unavailable")
                        body = bytearray()
                        async for chunk in response.aiter_bytes():
                            body.extend(chunk)
                            if len(body) > max_bytes:
                                raise MarketError("upstream_too_large")
                        return bytes(body)
                except httpx.RemoteProtocolError:
                    # The upstream edge occasionally resets a request it would
                    # otherwise serve; one retry after a pause recovers those
                    # windows instead of degrading the card.
                    if attempt:
                        raise
                    await asyncio.sleep(1.0)

    async def _json(self, url: str, params: dict, headers: dict | None = None,
                    timeout: float | None = None) -> dict:
        return json.loads(await self._text(url, params, headers, timeout=timeout))

    async def _eastmoney_json(self, url: str, params: dict, headers: dict | None = None) -> dict:
        async with self.eastmoney_gate:
            delay = self.eastmoney_next - time.monotonic()
            if delay > 0:
                await asyncio.sleep(delay)
            # Reserve the slot before the request leaves, so a failed call does
            # not let the next one hammer the same window.
            self.eastmoney_next = time.monotonic() + EASTMONEY_GAP_S
        return await self._json(url, params, headers)

    async def _fetch(self, item: Instrument, period: str) -> dict:
        if item.source == "tencent":
            base = "https://web.ifzq.gtimg.cn/appstock/app"
            if period == "1d":
                raw = await self._json(base + "/minute/query", {"code": item.symbol})
                data = raw["data"][item.symbol]
                day = data["data"]["date"]
                points = []
                for row in data["data"]["data"]:
                    parts = row.split()
                    if len(parts) >= 2:
                        points.append([stamp(day + parts[0], item), parts[1]])
            else:
                raw = await self._json(base + "/fqkline/get", {
                    "param": f"{item.symbol},day,,,{32 if period == '1mo' else 370},"
                })
                data = raw["data"][item.symbol]
                rows = data.get("day", [])
                close_time = "1600" if item.group == "hk" else "1500"
                points = [[stamp(row[0] + close_time, item), row[2]] for row in rows if len(row) >= 3]
                if points:
                    last = datetime.fromtimestamp(points[-1][0], ZoneInfo(item.timezone))
                    cutoff = (last - timedelta(days=31 if period == "1mo" else 366)).timestamp()
                    points = [p for p in points if p[0] >= cutoff]
            qt = data["qt"][item.symbol]
            if item.unit == "share":
                item = replace(item, name=str(qt[1])[:60])
            return snapshot(item, period, qt[3], qt[4], stamp(qt[30], item), points, qt[33], qt[34])
        if item.group == "us" and period == "1d" and item.id in CNBC_US:
            # CNBC carries the live US session and answers from networks that
            # block Yahoo; as an intraday source it beats Yahoo, whose range=1d
            # window collapses to one point outside the session.
            try:
                return await self._fetch_cnbc_us(item, period)
            except UPSTREAM_ERRORS:
                pass
        try:
            return await self._fetch_yahoo(item, period)
        except UPSTREAM_ERRORS:
            # Yahoo is unreachable from some networks. Keep the instrument list
            # unchanged and fall back to a quote-compatible feed in that case.
            if item.group == "commodity":
                return await self._fetch_sina_futures(item, period)
            if item.group == "us":
                if item.unit == "share":
                    # Eastmoney/Tencent have the intraday line but Tencent only
                    # serves two daily bars for US stocks, so use Sina for
                    # month/year and the others for 1d.
                    if period == "1d":
                        try:
                            return await self._fetch_eastmoney_us(item, period)
                        except UPSTREAM_ERRORS:
                            try:
                                return await self._fetch_tencent_us(item, period)
                            except UPSTREAM_ERRORS:
                                return await self._fetch_sina_us_stock(item, period)
                    try:
                        return await self._fetch_sina_us_stock(item, period)
                    except UPSTREAM_ERRORS:
                        return await self._fetch_tencent_us(item, period)
                if period == "1d":
                    # Eastmoney's edge now drops cloud IPs, so it is only the
                    # fallback behind CNBC.
                    try:
                        return await self._fetch_eastmoney_us(item, period)
                    except UPSTREAM_ERRORS:
                        pass
                return await self._fetch_tencent_us(item, period)
            raise

    async def _fetch_yahoo(self, item: Instrument, period: str) -> dict:
        raw = await self._json(
            "https://query1.finance.yahoo.com/v8/finance/chart/" + quote(item.symbol, safe=""),
            # range=1d is a calendar day in the exchange zone: before the open it
            # holds a single point. Ask for five days and keep the last session.
            {"range": "5d" if period == "1d" else period, "interval": PERIODS[period]},
        )
        data = raw["chart"]["result"][0]
        meta = data["meta"]
        if item.unit == "share":
            item = replace(item, name=str(meta.get("shortName") or meta.get("longName") or item.name)[:60],
                           currency=str(meta.get("currency") or item.currency)[:8])
        closes = data["indicators"]["quote"][0].get("close", [])
        points = list(zip(data.get("timestamp", []), closes))
        if period == "1d":
            points = last_session(points, item)
        # The quote card always shows the daily change, so compare with the
        # previous session close. chartPreviousClose is only a 1d approximation;
        # on a month/year it is the period baseline, never yesterday's close.
        previous = meta.get("previousClose")
        if previous is None and period == "1d":
            previous = meta.get("chartPreviousClose")
        return snapshot(item, period, meta.get("regularMarketPrice"), previous,
                        meta.get("regularMarketTime"), points, meta.get("regularMarketDayHigh"),
                        meta.get("regularMarketDayLow"))

    async def _fetch_cnbc_us(self, item: Instrument, period: str) -> dict:
        if period != "1d":
            raise MarketError("upstream_unavailable")
        hit = self.cnbc_cache.get(item.id)
        if hit and time.monotonic() - hit[0] < CNBC_TTL_S:
            return {**hit[1], "points": [list(p) for p in hit[1]["points"]]}
        raw = await self._json(CNBC_CHART, {"symbol": CNBC_US.get(item.id, item.symbol),
                                            "events": "1", "interval": "1",
                                            "lang": "en", "region": "USA"},
                               timeout=CNBC_TIMEOUT_S)
        bars = (raw.get("barData") or {}).get("priceBars") or []
        sessions: dict[str, list] = {}
        for row in bars:
            when = str(row.get("tradeTime") or "")
            close = number(row.get("close"))
            if len(when) != 14 or close is None:
                continue
            clock = when[8:12]
            # The feed also carries 04:00-20:00 pre/post prints; the line and
            # the day range must stay inside the regular session.
            if clock < "0930" or clock > "1600":
                continue
            sessions.setdefault(when[:8], []).append(
                (clock, close, number(row.get("high")), number(row.get("low"))))
        days = sorted(sessions)
        if not days or len(sessions[days[-1]]) < 2:
            raise MarketError("upstream_unavailable")
        last = sorted(sessions[days[-1]])
        previous = sorted(sessions[days[-2]])[-1][1] if len(days) > 1 else None
        points = [[stamp_zone(days[-1] + clock + "00", item.timezone), close] for clock, close, _, _ in last]
        highs = [h for _, _, h, _ in last if h is not None]
        lows = [l for _, _, _, l in last if l is not None]
        item = replace(item, source="cnbc")
        value = snapshot(item, period, last[-1][1], previous, points[-1][0], points,
                         max(highs, default=None), min(lows, default=None))
        self.cnbc_cache[item.id] = (time.monotonic(), value)
        if len(self.cnbc_cache) > MAX_CACHE_KEYS:
            self.cnbc_cache.pop(next(iter(self.cnbc_cache)))
        return {**value, "points": [list(p) for p in value["points"]]}

    async def _fetch_eastmoney_us(self, item: Instrument, period: str) -> dict:
        if period != "1d":
            raise MarketError("upstream_unavailable")
        candidates = (EASTMONEY_US[item.id],) if item.id in EASTMONEY_US else (
            "105." + item.symbol, "106." + item.symbol)
        for secid in candidates:
            try:
                raw = await self._eastmoney_json(
                    "https://push2his.eastmoney.com/api/qt/stock/trends2/get",
                    {"secid": secid, "fields1": "f1,f2,f3,f4,f5,f6,f7,f8",
                     "fields2": "f51,f52,f53,f54,f55,f56,f57,f58", "ndays": "1", "iscr": "0",
                     # Public quote token; without it the host drops the connection.
                     "ut": "fa5fd1943c7b386f172d6893dbfba10b"},
                    {"Referer": "https://quote.eastmoney.com/"})
            except UPSTREAM_ERRORS as exc:
                # The US board degrades to a quote-only card when this fails,
                # so say why: Eastmoney's edge drops cloud IPs, and without
                # this line the loss of the intraday line looks like thin air.
                log.info("eastmoney %s (%s) %s failed: %s: %s", item.id, secid, period,
                         type(exc).__name__, exc)
                continue
            data = raw.get("data") or {}
            points, highs, lows = [], [], []
            for row in data.get("trends") or []:
                parts = str(row).split(",")
                if len(parts) >= 5:
                    # Timestamps are Beijing time regardless of the venue.
                    points.append([stamp_zone(parts[0], "Asia/Shanghai"), parts[2]])
                    highs.append(number(parts[3]))
                    lows.append(number(parts[4]))
            if len(points) < 2:
                continue
            item = replace(item, source="eastmoney")
            if item.unit == "share" and data.get("name"):
                item = replace(item, name=str(data["name"])[:60], currency="USD")
            high = max((v for v in highs if v is not None), default=None)
            low = min((v for v in lows if v is not None), default=None)
            return snapshot(item, period, points[-1][1], data.get("preClose"), points[-1][0], points,
                            high, low)
        raise MarketError("upstream_unavailable")

    async def _fetch_tencent_us(self, item: Instrument, period: str) -> dict:
        code = TENCENT_US.get(item.symbol, "us" + item.symbol.removeprefix("US."))
        item = replace(item, source="tencent")
        base = "https://web.ifzq.gtimg.cn/appstock/app"
        if period == "1d":
            raw = await self._json(base + "/minute/query", {"code": code})
            node = next(v for v in raw["data"].values() if isinstance(v, dict))
            day = str(node.get("data", {}).get("date") or "").replace("-", "")
            rows = node.get("data", {}).get("data", [])
        else:
            raw = await self._json(base + "/kline/kline", {
                "param": f"{code},day,,,{32 if period == '1mo' else 370}"
            })
            node = next(v for v in raw["data"].values() if isinstance(v, dict))
            rows = node.get("day", [])
        qt = node.get("qt", {})
        if not qt:
            raise MarketError("invalid_upstream")
        quote_values = next(iter(qt.values()))
        if item.unit == "share":
            currency = quote_values[35] if len(quote_values) > 35 else None
            item = replace(item, name=str(quote_values[1])[:60],
                           currency=str(currency or item.currency)[:8])
        points = []
        if period == "1d":
            if not day:
                day = str(quote_values[30]).split(" ")[0].replace("-", "")
            for row in rows:
                parts = str(row).split()
                if len(parts) >= 2 and len(day) == 8:
                    points.append([stamp(day + parts[0], item), parts[1]])
        else:
            points = [[stamp(str(row[0]).replace("-", "") + "1600", item), row[2]]
                      for row in rows if len(row) >= 3]
            if points:
                last = datetime.fromtimestamp(points[-1][0], ZoneInfo(item.timezone))
                cutoff = (last - timedelta(days=31 if period == "1mo" else 366)).timestamp()
                points = [p for p in points if p[0] >= cutoff]
        return snapshot(item, period, quote_values[3], quote_values[4],
                        stamp(str(quote_values[30]), item), points,
                        quote_values[33], quote_values[34])

    async def _fetch_sina_us_stock(self, item: Instrument, period: str) -> dict:
        ticker = item.symbol
        item = replace(item, source="sina")
        headers = {"Referer": "https://finance.sina.com.cn/"}
        quote_body = await self._text("https://hq.sinajs.cn/list=gb_" + ticker.lower(), headers=headers)
        quote_text = quote_body.decode("gb18030", "replace")
        match = re.search(r'hq_str_gb_[^=]+="([^"]*)"', quote_text)
        if not match:
            raise MarketError("invalid_upstream")
        fields = match[1].split(",")
        if len(fields) < 27 or number(fields[1]) is None:
            raise MarketError("invalid_upstream")
        item = replace(item, name=str(fields[0])[:60] or item.name, currency="USD")
        points = []
        if period != "1d":
            body = await self._text(
                "https://stock2.finance.sina.com.cn/usstock/api/jsonp.php/var%20_x=/"
                "US_MinKService.getDailyK",
                {"symbol": ticker, "___qn": "3"}, headers, max_bytes=MAX_DAILY_BYTES)
            points = [[stamp(str(row["d"]).replace("-", "") + "1600", item), row["c"]]
                      for row in jsonp(body) if len(str(row.get("d", ""))) == 10]
            if points:
                last = datetime.fromtimestamp(points[-1][0], ZoneInfo(item.timezone))
                cutoff = (last - timedelta(days=31 if period == "1mo" else 366)).timestamp()
                points = [p for p in points if p[0] >= cutoff]
        return snapshot(item, period, fields[1], fields[26], stamp(fields[3], item),
                        points, fields[6], fields[7])

    async def _fetch_sina_futures(self, item: Instrument, period: str) -> dict:
        symbol = SINA_FUTURES.get(item.id)
        if symbol is None:
            raise MarketError("upstream_unavailable")
        item = replace(item, source="sina")
        headers = {"Referer": "https://finance.sina.com.cn/"}
        quote_body = await self._text("https://hq.sinajs.cn/list=hf_" + symbol, headers=headers)
        quote_text = quote_body.decode("gb18030", "replace")
        match = re.search(r'hq_str_hf_[A-Z]+="([^"]*)"', quote_text)
        if not match:
            raise MarketError("invalid_upstream")
        fields = match[1].split(",")
        if len(fields) < 14:
            raise MarketError("invalid_upstream")
        as_of = stamp(fields[12].replace("-", "") + fields[6].replace(":", ""), item)
        points = []
        if period == "1d":
            body = await self._text(
                "https://stock2.finance.sina.com.cn/futures/api/jsonp.php/var%20_x=/"
                "GlobalFuturesService.getGlobalFuturesMinLine",
                {"symbol": symbol}, headers)
            first_date = ""
            for row in jsonp(body).get("minLine_1d", []):
                if len(row) < 2:
                    continue
                if re.fullmatch(r"\d{2}:\d{2}", str(row[0])):
                    when = str(row[-1]) if len(row) > 5 else ""
                    day = when.split(" ")[0].replace("-", "") if " " in when else first_date
                    clock = str(row[0]).replace(":", "")
                else:
                    day, clock = str(row[0]).replace("-", ""), str(row[4]).replace(":", "")
                if len(day) == 8 and len(clock) == 4:
                    first_date = day
                    points.append([stamp(day + clock, item), row[1]])
        else:
            # Full history since the contract's inception (WTI is over 1 MB).
            body = await self._text(
                "https://stock2.finance.sina.com.cn/futures/api/jsonp.php/var%20_x=/"
                "GlobalFuturesService.getGlobalFuturesDailyKLine",
                {"symbol": symbol}, headers, max_bytes=MAX_DAILY_BYTES)
            points = [[stamp(str(row["date"]).replace("-", "") + "1600", item), row["close"]]
                      for row in jsonp(body) if len(str(row.get("date", ""))) == 10]
            if points:
                last = datetime.fromtimestamp(points[-1][0], ZoneInfo(item.timezone))
                cutoff = (last - timedelta(days=31 if period == "1mo" else 366)).timestamp()
                points = [p for p in points if p[0] >= cutoff]
        return snapshot(item, period, fields[0], fields[7], as_of, points, fields[4], fields[5])

    def _view(self, item: Instrument, period: str, error: str = "") -> dict:
        entry = self.cache.get((item.id, period))
        age = time.monotonic() - entry[0] if entry else 0
        if entry and age <= self.settings.market_stale_s:
            return {**entry[1], "stale": bool(error), "error": error, "cacheAge": int(age)}
        return {**metadata(item), "available": False, "period": period,
                "stale": False, "error": error or "no_data", "points": []}

    async def get(self, instrument: str, period: str, *, stock: bool = False) -> dict:
        if period not in PERIODS:
            raise MarketError("invalid_market_query", 400)
        item = resolve(instrument, allow_indexes=not stock)
        key = (item.id, period)
        async with self.locks[hash(key) % len(self.locks)]:
            now = time.monotonic()
            ttl = self.settings.market_quote_ttl_s if period == "1d" else self.settings.market_chart_ttl_s
            entry = self.cache.get(key)
            if entry and now - entry[0] < ttl:
                self.cache.move_to_end(key)
                return self._view(item, period)
            if now < self.retry_after.get(key, 0):
                return self._view(item, period, "retry_later")
            while self.misses and now - self.misses[0] >= 60:
                self.misses.popleft()
            if len(self.misses) >= 60:
                return self._view(item, period, "rate_limited")
            self.misses.append(now)
            try:
                # The total deadline includes semaphore wait and streamed reads.
                async with asyncio.timeout(self.settings.market_timeout_s):
                    async with self.semaphore:
                        value = await self._fetch(item, period)
                self.cache[key] = (time.monotonic(), {**value, "fetchedAt": int(time.time())})
                self.cache.move_to_end(key)
                if len(self.cache) > MAX_CACHE_KEYS:
                    self.cache.popitem(last=False)
                self.retry_after.pop(key, None)
                return self._view(item, period)
            except (MarketError, httpx.HTTPError, httpx.InvalidURL, TimeoutError, ValueError, KeyError,
                    TypeError, IndexError, AttributeError, OverflowError, StopIteration, RuntimeError) as exc:
                # StopIteration surfaces as RuntimeError inside a coroutine; an
                # upstream that returns an empty data node must degrade the
                # card, not fail the endpoint with a 500.
                code = exc.code if isinstance(exc, MarketError) else "upstream_unavailable"
                # Upstream chains are silent by design (the card degrades
                # instead of failing the request); without this line a block
                # page or a dropped connection is invisible in journald.
                log.warning("market %s/%s degraded: %s: %s", item.id, period,
                            type(exc).__name__, exc)
                self.retry_after[key] = time.monotonic() + 30
                self.retry_after.move_to_end(key)
                if len(self.retry_after) > MAX_CACHE_KEYS:
                    self.retry_after.popitem(last=False)
                return self._view(item, period, code)

    async def board(self, group: str) -> dict:
        if group not in GROUPS:
            raise MarketError("invalid_market_group", 400)
        values = await asyncio.gather(*(self.get(i.id, "1d") for i in INSTRUMENTS if i.group == group))
        cards = [{**v, "points": compact(v["points"], 24)} for v in values]
        return {"ok": True, "group": group, "cards": cards}

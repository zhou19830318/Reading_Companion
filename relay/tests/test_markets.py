"""Offline contract tests; no public feed or live price is required."""

import asyncio
import time
from datetime import datetime
from zoneinfo import ZoneInfo

import httpx
import pytest
from fastapi.testclient import TestClient

from relay.config import Settings, reload_settings
from relay.main import app
from relay.markets import (BY_ID, MAX_CACHE_KEYS, MAX_DAILY_BYTES, MarketError, MarketStore, compact, resolve,
                           snapshot)

AUTH = {"Authorization": "Bearer test-device-token", "X-Device-Id": "debug-client"}


def sample(item=BY_ID["sse"], period="1d"):
    return snapshot(item, period, 102, 100, 1700000000,
                    [[1700000000 + i * 60, 100 + i % 7] for i in range(400)])


@pytest.mark.parametrize(("code", "expected"), [
    ("600519", "SH600519"), ("000001", "SZ000001"), ("300750", "SZ300750"),
    ("00700", "HK00700"), ("0700", "HK00700"), ("hk00700", "HK00700"),
    ("aapl", "US.AAPL"), ("BRK.B", "US.BRK-B"), ("US.MSFT", "US.MSFT"),
    (" sh600519 ", "SH600519"),
])
def test_resolve_stock(code, expected):
    assert resolve(code, allow_indexes=False).id == expected


@pytest.mark.parametrize("code", ["", "../../etc", "AAPL&url=x", "https://x", "123456", "123", "中文", "A" * 17])
def test_reject_invalid_codes(code):
    with pytest.raises(MarketError):
        resolve(code, allow_indexes=False)


def test_stock_and_index_are_unambiguous():
    assert resolve("sse").symbol == "sh000001"
    assert resolve("000001", False).symbol == "sz000001"
    assert resolve("dow", False).symbol == "DOW"


def test_compaction_preserves_endpoints_and_extrema():
    points = [[i, 100] for i in range(1000)]
    points[41][1], points[387][1] = -200, 999
    for limit in (24, 96):
        result = compact(points, limit)
        assert len(result) <= limit
        assert result == sorted(result)
        assert result[0] == points[0] and result[-1] == points[-1]
        assert min(p[1] for p in result) == -200
        assert max(p[1] for p in result) == 999


def test_null_nan_and_duplicate_points():
    card = snapshot(BY_ID["sse"], "1d", 2, 0, 1700000000,
                    [[1700000000, None], [1700000001, float("nan")],
                     [1700000002, 1], [1700000002, 2]])
    assert card["points"] == [[1700000002, 2]]
    assert card["changePct"] is None
    assert card["chartAvailable"] is False
    with pytest.raises(MarketError):
        snapshot(BY_ID["sse"], "1d", float("inf"), 1, 1700000000, [])


def test_cache_singleflight_stale_expiry_and_negative_cache(monkeypatch):
    store = MarketStore(Settings())
    calls = 0

    async def fetch(item, period):
        nonlocal calls
        calls += 1
        await asyncio.sleep(0)
        return sample(item, period)

    monkeypatch.setattr(store, "_fetch", fetch)

    async def scenario():
        result = await asyncio.gather(*(store.get("sse", "1d") for _ in range(8)))
        assert calls == 1 and all(c["available"] for c in result)
        ts, data = store.cache[("sse", "1d")]
        store.cache[("sse", "1d")] = (ts - 120, data)

        async def fail(item, period):
            nonlocal calls
            calls += 1
            raise MarketError("rate_limited")

        monkeypatch.setattr(store, "_fetch", fail)
        assert (await store.get("sse", "1d"))["stale"] is True
        assert (await store.get("sse", "1d"))["error"] == "retry_later"
        assert calls == 2
        store.cache[("sse", "1d")] = (ts - 90000, data)
        assert (await store.get("sse", "1d"))["available"] is False
    asyncio.run(scenario())


def test_cache_and_retry_maps_are_bounded(monkeypatch):
    store = MarketStore(Settings())

    async def fetch(item, period):
        return sample(item, period)

    monkeypatch.setattr(store, "_fetch", fetch)

    async def scenario():
        for n in range(MAX_CACHE_KEYS + 10):
            store.misses.clear()
            await store.get(f"SH{600000 + n}", "1d")
        assert len(store.cache) == MAX_CACHE_KEYS
        assert len(store.locks) == 32
    asyncio.run(scenario())


def test_total_timeout(monkeypatch):
    store = MarketStore(Settings())
    store.settings.market_timeout_s = 0.01

    async def fetch(item, period):
        await asyncio.sleep(1)

    monkeypatch.setattr(store, "_fetch", fetch)
    result = asyncio.run(store.get("sse", "1d"))
    assert result["available"] is False


def test_tencent_stock_name_and_raw_daily_series(monkeypatch):
    store = MarketStore(Settings())
    qt = [""] * 35
    qt[1], qt[3], qt[4], qt[30] = "Test Stock", "102", "100", "20261112112145"
    qt[33], qt[34] = "105", "99"

    async def json_data(url, params):
        assert params["param"].endswith(",")  # unadjusted, same basis as quote
        return {"data": {"sh600519": {"qt": {"sh600519": qt},
                                    "day": [["2026-11-11", "100", "101"],
                                            ["2026-11-12", "101", "102"]]}}}

    monkeypatch.setattr(store, "_json", json_data)
    result = asyncio.run(store.get("600519", "1mo", stock=True))
    assert result["name"] == "Test Stock"
    assert result["changePct"] == 2
    assert len(result["points"]) == 2


def test_yahoo_month_baseline_is_not_daily_change(monkeypatch):
    store = MarketStore(Settings())

    async def json_data(url, params):
        return {"chart": {"result": [{
            "meta": {"regularMarketPrice": 102, "regularMarketTime": 1700000000,
                     "chartPreviousClose": 50, "shortName": "Apple", "currency": "USD"},
            "timestamp": [1700000000, 1700000060],
            "indicators": {"quote": [{"close": [101, 102]}]},
        }]}}

    monkeypatch.setattr(store, "_json", json_data)
    result = asyncio.run(store.get("AAPL", "1mo", stock=True))
    assert result["name"] == "Apple" and result["changePct"] is None


def new_york(day, hour, minute):
    return int(datetime(2026, 11, day, hour, minute, tzinfo=ZoneInfo("America/New_York")).timestamp())


def test_yahoo_intraday_uses_last_full_session(monkeypatch):
    store = MarketStore(Settings())
    seen = {}

    async def json_data(url, params):
        seen["range"] = params["range"]
        return {"chart": {"result": [{
            "meta": {"regularMarketPrice": 102, "regularMarketTime": new_york(12, 0, 5),
                     "previousClose": 100, "shortName": "S&P 500"},
            "timestamp": [new_york(10, 15, 0), new_york(10, 15, 5), new_york(10, 15, 10),
                          new_york(12, 0, 0)],
            "indicators": {"quote": [{"close": [100, 101, 102, None]}]},
        }]}}

    monkeypatch.setattr(store, "_json", json_data)
    result = asyncio.run(store.get("sp500", "1d"))
    assert seen["range"] == "5d"
    assert result["points"] == [[new_york(10, 15, 0), 100], [new_york(10, 15, 5), 101],
                               [new_york(10, 15, 10), 102]]
    assert result["changePct"] == 2


def test_yahoo_long_range_keeps_daily_change(monkeypatch):
    store = MarketStore(Settings())

    async def json_data(url, params):
        return {"chart": {"result": [{
            "meta": {"regularMarketPrice": 110, "regularMarketTime": 1700000000,
                     "previousClose": 100, "chartPreviousClose": 50, "shortName": "Apple"},
            "timestamp": [1700000000, 1700000060],
            "indicators": {"quote": [{"close": [105, 110]}]},
        }]}}

    monkeypatch.setattr(store, "_json", json_data)
    result = asyncio.run(store.get("AAPL", "1y", stock=True))
    assert result["change"] == 10 and result["changePct"] == 10


def test_unusable_http_environment_degrades(monkeypatch):
    store = MarketStore(Settings())

    def broken(**kwargs):
        raise httpx.InvalidURL("bad proxy entry")

    monkeypatch.setattr(httpx, "AsyncClient", broken)
    result = asyncio.run(store.get("sse", "1d"))
    assert not result["available"] and result["error"] == "upstream_unavailable"


def test_http_rate_limit_is_not_data(monkeypatch):
    store = MarketStore(Settings())
    real_client = httpx.AsyncClient
    transport = httpx.MockTransport(lambda request: httpx.Response(429))
    monkeypatch.setattr(httpx, "AsyncClient", lambda **kw: real_client(transport=transport, **kw))
    result = asyncio.run(store.get("sse", "1d"))
    assert not result["available"]
    assert result["error"] == "rate_limited"


def test_upstream_requests_identify_as_browser(monkeypatch):
    store = MarketStore(Settings())
    real_client = httpx.AsyncClient
    seen = {}

    def handler(request):
        seen["ua"] = request.headers.get("User-Agent", "")
        seen["referer"] = request.headers.get("Referer", "")
        return httpx.Response(200, content=b"ok")

    transport = httpx.MockTransport(handler)
    monkeypatch.setattr(httpx, "AsyncClient", lambda **kw: real_client(transport=transport, **kw))
    body = asyncio.run(store._text("https://push2his.eastmoney.com/x", {"a": "b"},
                                   {"Referer": "https://quote.eastmoney.com/"}))
    assert body == b"ok"
    # Eastmoney's edge resets requests whose agent looks scripted, so the
    # default must stay a real browser identity and keep the caller's extra
    # headers.
    assert "Mozilla/5.0" in seen["ua"] and "Chrome/" in seen["ua"]
    assert seen["referer"] == "https://quote.eastmoney.com/"


def test_upstream_retries_one_connection_reset(monkeypatch):
    store = MarketStore(Settings())
    real_client = httpx.AsyncClient
    calls = []

    def handler(request):
        calls.append(1)
        if len(calls) == 1:
            raise httpx.RemoteProtocolError("Server disconnected without sending a response.")
        return httpx.Response(200, content=b"ok")

    transport = httpx.MockTransport(handler)
    monkeypatch.setattr(httpx, "AsyncClient", lambda **kw: real_client(transport=transport, **kw))
    monkeypatch.setattr(asyncio, "sleep", lambda _delay: _noop())
    assert asyncio.run(store._text("https://push2his.eastmoney.com/x")) == b"ok"
    assert len(calls) == 2


async def _noop():
    return None


@pytest.fixture
def client(monkeypatch):
    monkeypatch.setenv("ALLOWED_DEVICE_IDS", "debug-client")
    monkeypatch.setenv("RELAY_DEVICE_TOKENS", "")
    reload_settings()

    async def fetch(self, item, period):
        if item.id == "szse":
            raise MarketError("upstream_unavailable")
        return sample(item, period)

    monkeypatch.setattr(MarketStore, "_fetch", fetch)
    with TestClient(app) as c:
        yield c
    reload_settings()


@pytest.mark.parametrize("endpoint", ["board", "chart", "lookup"])
def test_device_auth(client, endpoint):
    assert client.get("/api/v1/markets/" + endpoint).status_code == 401


def test_board_partial_failure_and_size(client):
    response = client.get("/api/v1/markets/board", headers=AUTH)
    assert response.status_code == 200
    cards = response.json()["cards"]
    assert len(cards) == 4 and not cards[1]["available"] and cards[0]["available"]
    assert all(len(c["points"]) <= 24 for c in cards)
    assert len(response.content) < 12288


def test_lookup_and_chart(client):
    response = client.get("/api/v1/markets/lookup?code=600519", headers=AUTH)
    assert response.json()["card"]["id"] == "SH600519"
    response = client.get("/api/v1/markets/chart?instrument=US.AAPL&period=1y", headers=AUTH)
    assert len(response.json()["card"]["points"]) <= 96
    assert len(response.content) < 12288


@pytest.mark.parametrize("query", [
    "lookup?code=../../etc", "chart?period=10y", "board?group=bogus",
])
def test_bad_query(client, query):
    assert client.get("/api/v1/markets/" + query, headers=AUTH).status_code == 400


def test_yahoo_failure_falls_back_to_tencent_us_index(monkeypatch):
    store = MarketStore(Settings())
    qt = [""] * 35
    qt[1], qt[3], qt[4], qt[30] = "S&P 500", "7619.38", "7520.93", "20261111164200"
    qt[33], qt[34] = "7625.10", "7500.20"

    async def yahoo_failed(item, period):
        raise MarketError("upstream_unavailable")

    async def json_data(url, params):
        return {"data": {"usINX": {"qt": {"usINX": qt},
                                    "day": [["2026-11-10", "7500", "7520"],
                                            ["2026-11-11", "7520", "7619"]]}}}

    monkeypatch.setattr(store, "_fetch_yahoo", yahoo_failed)
    monkeypatch.setattr(store, "_json", json_data)
    result = asyncio.run(store.get("sp500", "1mo"))
    assert result["available"] and result["points"]
    assert result["price"] == 7619.38 and result["previousClose"] == 7520.93


def test_yahoo_failure_falls_back_to_sina_futures_and_stocks(monkeypatch):
    store = MarketStore(Settings())

    async def yahoo_failed(item, period):
        raise MarketError("upstream_unavailable")

    async def text_data(url, params=None, headers=None, max_bytes=None):
        if "hf_GC" in url:
            return ('var hq_str_hf_GC="4012.5,,4013,4014,4015,4005,12:34:56,'
                    '4008,4010,0,1,4,2026-11-11,Gold";').encode()
        if "GlobalFuturesService" in url:
            return (b'var _x=([{"date":"2026-11-10","close":"4000"},'
                    b'{"date":"2026-11-11","close":"4008"}]);')
        if "US_MinKService.getDailyK" in url:
            return (b'var _x=([{"d":"2026-11-10","c":"348.51"},'
                    b'{"d":"2026-11-11","c":"355.63"}]);')
        return (b'var hq_str_gb_aapl="Apple,355.63,0.00,2026-11-11 16:42:00,'
                b'2.04,0.58,356.00,349.00,350.00,348.51,100,0,0,0,0,0,0,0,0,0,'
                b'0,0,0,0,0,0,348.51,0,0,0";')

    monkeypatch.setattr(store, "_fetch_yahoo", yahoo_failed)
    monkeypatch.setattr(store, "_text", text_data)
    gold = asyncio.run(store.get("gold", "1mo"))
    assert gold["available"] and len(gold["points"]) == 2
    assert gold["price"] == 4012.5 and gold["previousClose"] == 4008
    assert gold["source"] == "sina"
    aapl = asyncio.run(store.get("AAPL", "1mo", stock=True))
    assert aapl["available"] and aapl["name"] == "Apple"
    assert aapl["price"] == 355.63 and len(aapl["points"]) == 2
    assert aapl["source"] == "sina"


def test_us_stock_month_uses_sina_daily_history(monkeypatch):
    store = MarketStore(Settings())

    async def yahoo_failed(item, period):
        raise MarketError("upstream_unavailable")

    async def json_data(url, params):
        raise AssertionError("Tencent only serves two daily bars for US stocks")

    async def text_data(url, params=None, headers=None, max_bytes=None):
        if "hq.sinajs.cn" in url:
            return (b'var hq_str_gb_aapl="Apple,355.63,0.00,2026-11-11 16:42:00,2.04,0.58,'
                    b'356.00,349.00,350.00,348.51,100,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,348.51,0,0,0";')
        assert "US_MinKService.getDailyK" in url
        return b'var _x=([{"d":"2026-11-10","c":"348.51"},{"d":"2026-11-11","c":"355.63"}]);'

    monkeypatch.setattr(store, "_fetch_yahoo", yahoo_failed)
    monkeypatch.setattr(store, "_json", json_data)
    monkeypatch.setattr(store, "_text", text_data)
    card = asyncio.run(store.get("AAPL", "1mo", stock=True))
    assert card["source"] == "sina" and len(card["points"]) == 2


def test_us_index_intraday_prefers_cnbc(monkeypatch):
    store = MarketStore(Settings())
    calls = []

    async def yahoo_failed(item, period):
        raise MarketError("upstream_unavailable")

    async def json_data(url, params, headers=None, timeout=None):
        assert "ts-api.cnbc.com" in url
        calls.append(params["symbol"])
        # Two sessions plus pre/post prints: the feed spans 04:00-20:00, while
        # the chart must stay inside 09:30-16:00 New York.
        return {"barData": {"priceBars": [
            {"tradeTime": "20261110155900", "close": "7500.00", "high": "7510", "low": "7490"},
            {"tradeTime": "20261111080000", "close": "7550.00", "high": "7555", "low": "7545"},
            {"tradeTime": "20261111093100", "close": "7580.50", "high": "7581", "low": "7579"},
            {"tradeTime": "20261111160000", "close": "7619.00", "high": "7620", "low": "7500.20"},
            {"tradeTime": "20261111180000", "close": "7625.00", "high": "7626", "low": "7624"},
        ]}}

    monkeypatch.setattr(store, "_fetch_yahoo", yahoo_failed)
    monkeypatch.setattr(store, "_json", json_data)
    card = asyncio.run(store.get("sp500", "1d"))
    assert calls == [".SPX"]
    assert card["source"] == "cnbc"
    assert card["price"] == 7619 and card["previousClose"] == 7500
    assert card["changePct"] == pytest.approx((7619 - 7500) / 7500 * 100)
    assert card["high"] == 7620 and card["low"] == 7500.2
    assert [p[1] for p in card["points"]] == [7580.5, 7619.0]
    assert card["points"][0][0] == int(
        datetime(2026, 11, 11, 9, 31, tzinfo=ZoneInfo("America/New_York")).timestamp())

    cached = asyncio.run(store._fetch_cnbc_us(BY_ID["sp500"], "1d"))
    assert calls == [".SPX"], "the parsed session is reused for five minutes"
    assert [p[1] for p in cached["points"]] == [7580.5, 7619.0]


def test_us_index_intraday_uses_eastmoney(monkeypatch):
    store = MarketStore(Settings())
    seen = {}

    async def yahoo_failed(item, period):
        raise MarketError("upstream_unavailable")

    async def cnbc_failed(item, period):
        raise MarketError("upstream_unavailable")

    async def json_data(url, params, headers=None, timeout=None):
        assert "push2his.eastmoney.com" in url
        seen["secid"] = params["secid"]
        return {"data": {"name": "标普500", "preClose": 7500, "trends": [
            "2026-11-11 22:30,7580,7581,7582,7579,1,0,7580",
            "2026-11-12 05:00,7618,7619,7620,7617,1,0,7618",
        ]}}

    monkeypatch.setattr(store, "_fetch_yahoo", yahoo_failed)
    monkeypatch.setattr(store, "_fetch_cnbc_us", cnbc_failed)
    monkeypatch.setattr(store, "_json", json_data)
    card = asyncio.run(store.get("sp500", "1d"))
    assert seen["secid"] == "100.SPX"
    assert card["source"] == "eastmoney"
    assert card["price"] == 7619 and card["previousClose"] == 7500
    assert card["changePct"] == pytest.approx((7619 - 7500) / 7500 * 100)
    assert card["points"][0][0] == int(
        datetime(2026, 11, 11, 22, 30, tzinfo=ZoneInfo("Asia/Shanghai")).timestamp())
    assert card["high"] == 7620 and card["low"] == 7579


def test_us_stock_intraday_tries_nasdaq_then_nyse(monkeypatch):
    store = MarketStore(Settings())
    seen = []

    async def yahoo_failed(item, period):
        raise MarketError("upstream_unavailable")

    async def json_data(url, params, headers=None):
        seen.append(params["secid"])
        if params["secid"] == "106.JPM":
            return {"data": {"name": "摩根大通", "preClose": 340, "trends": [
                "2026-11-11 22:30,341,341.5,342,340.5,1,0,341",
                "2026-11-12 05:00,342,342.5,343,342,1,0,342",
            ]}}
        return {"data": None}

    monkeypatch.setattr(store, "_fetch_yahoo", yahoo_failed)
    monkeypatch.setattr(store, "_json", json_data)
    card = asyncio.run(store.get("JPM", "1d", stock=True))
    assert seen == ["105.JPM", "106.JPM"]
    assert card["source"] == "eastmoney" and card["name"] == "摩根大通"
    assert len(card["points"]) == 2


def test_futures_daily_fallback_allows_full_history(monkeypatch):
    store = MarketStore(Settings())
    limits = []

    async def yahoo_failed(item, period):
        raise MarketError("upstream_unavailable")

    async def text_data(url, params=None, headers=None, max_bytes=None):
        if "hf_CL" in url:
            return (b'var hq_str_hf_CL="85.4,,85.5,86,84.5,84.1,12:34:56,84.6,4010,0,1,4,'
                    b'2026-11-11,Crude";')
        assert "GlobalFuturesService" in url
        limits.append(max_bytes)
        return b'var _x=([{"date":"2026-11-10","close":"84"},{"date":"2026-11-11","close":"85"}]);'

    monkeypatch.setattr(store, "_fetch_yahoo", yahoo_failed)
    monkeypatch.setattr(store, "_text", text_data)
    result = asyncio.run(store.get("wti", "1y"))
    assert result["available"] and result["source"] == "sina"
    assert result["points"] == [[(datetime(2026, 11, 10, 16, tzinfo=ZoneInfo("America/New_York")).timestamp()),
                                 84.0],
                                [(datetime(2026, 11, 11, 16, tzinfo=ZoneInfo("America/New_York")).timestamp()),
                                 85.0]]
    assert limits == [MAX_DAILY_BYTES]


def test_empty_upstream_data_degrades_instead_of_500(monkeypatch):
    store = MarketStore(Settings())

    async def yahoo_failed(item, period):
        raise MarketError("upstream_unavailable")

    async def text_data(url, params=None, headers=None, max_bytes=None):
        raise MarketError("upstream_unavailable")

    async def json_data(url, params, headers=None):
        # Tencent reports success with no instrument node; the fallback chain
        # walks into next() on an empty mapping (StopIteration -> RuntimeError).
        return {"data": {}}

    monkeypatch.setattr(store, "_fetch_yahoo", yahoo_failed)
    monkeypatch.setattr(store, "_text", text_data)
    monkeypatch.setattr(store, "_json", json_data)
    result = asyncio.run(store.get("sp500", "1mo"))
    assert not result["available"] and result["error"] == "upstream_unavailable"

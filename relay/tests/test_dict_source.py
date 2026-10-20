"""Offline dictionary cards: rendering, lookup, and the /api/v1/chat wiring."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from relay import dict_source
from relay.config import reload_settings
from relay.main import app

ADMIN = {"Authorization": "Bearer test-admin-token"}

CHARS = {
    "的": [
        ["de", "dì", "dí"],
        "白",
        8,
        ["de：用在词或词组后表明形容词性：美丽～。", "dì：箭靶的中心：众矢之～。"],
    ],
    "乐": [["lè", "yuè"], "丿", 5, ["lè：欢喜，快活。", "yuè：音乐。"]],
    "桛": [[], "木", 10, ["日本和字。"]],
}
WORDS = {"的": ["目的", "真的"], "乐": ["音乐", "快乐"]}


STROKES = {"的": "1,2;3,4|5,6;7,8"}


@pytest.fixture()
def data_dir(tmp_path):
    (tmp_path / "chars.json").write_text(json.dumps(CHARS, ensure_ascii=False), encoding="utf-8")
    (tmp_path / "words.json").write_text(json.dumps(WORDS, ensure_ascii=False), encoding="utf-8")
    (tmp_path / "strokes.json").write_text(json.dumps(STROKES, ensure_ascii=False), encoding="utf-8")
    (tmp_path / "META.json").write_text(json.dumps({"built_at": "test"}), encoding="utf-8")
    return tmp_path


@pytest.fixture(autouse=True)
def _reset_dict_cache():
    dict_source.reset_cache()
    yield
    dict_source.reset_cache()


@pytest.fixture()
def client(data_dir, monkeypatch):
    monkeypatch.setenv("DICT_SOURCE", "local")
    monkeypatch.setenv("DICT_DATA_DIR", str(data_dir))
    reload_settings()
    with TestClient(app) as c:
        yield c


def test_render_card_omits_empty_sections():
    card = dict_source.render_card("桛", [], "木", 10, ["日本和字。"], [])
    assert card == "【字】桛\n【部首笔画】木·10画\n【释义】日本和字。\n"


def test_render_card_substitutes_headword_placeholder():
    assert dict_source.render_card("科", [], "", 0, ["猫～。豆～。"], []) == "【字】科\n【释义】猫科。豆科。\n"


def test_resolve_data_dir_rules(data_dir):
    assert dict_source.resolve_data_dir(str(data_dir)) == data_dir
    assert dict_source.resolve_data_dir(str(data_dir / "nope")) is None
    # No explicit dir: the checkout copy (relay/dict-data) is a candidate.
    assert dict_source.resolve_data_dir("") is not None


def test_card_lookup(data_dir):
    src = dict_source.DictSource(data_dir)
    assert src.load()

    card = src.card("的")
    assert card.startswith("【字】的\n")
    assert "【拼音】de、dì、dí\n" in card
    assert "【部首笔画】白·8画\n" in card
    assert "【释义】de：用在词或词组后表明形容词性：美丽的。 dì：箭靶的中心：众矢之的。\n" in card
    assert "【组词】目的、真的\n" in card

    le = src.card("乐")
    assert "【拼音】lè、yuè\n" in le
    assert "【组词】音乐、快乐\n" in le
    assert dict_source.render_card("x", ["xx"], "", 0, [], []) == "【字】x\n【拼音】xx\n"

    missing = src.card("囧")
    assert missing == "【字】囧\n【释义】该字未收录于离线字典。\n"
    assert "【拼音】" not in missing

    assert src.card("你好世界") == "【字】无法识别\n【释义】请输入单个汉字。\n"
    assert src.card("N") == "【字】无法识别\n【释义】请输入单个汉字。\n"  # letter, not a hanzi
    assert src.card(" 桛 ") == "【字】桛\n【部首笔画】木·10画\n【释义】日本和字。\n"


def test_stroke_payload_rules(data_dir):
    src = dict_source.DictSource(data_dir)
    assert src.load()

    assert src.stroke_payload("的") == "1,2;3,4|5,6;7,8"
    assert src.stroke_payload(" 的 ") == "1,2;3,4|5,6;7,8"  # stripped like card()
    assert src.stroke_payload("乐") is None  # a card entry without stroke data
    assert src.stroke_payload("N") is None  # not a hanzi
    assert src.stroke_payload("你好") is None  # not a single character


def test_strokes_payload_is_optional(tmp_path):
    # A data dir without strokes.json still loads; every payload is None.
    (tmp_path / "chars.json").write_text(json.dumps(CHARS, ensure_ascii=False), encoding="utf-8")
    src = dict_source.DictSource(tmp_path)
    assert src.load()
    assert src.stroke_payload("的") is None
    assert src.card("的").startswith("【字】的\n")


def test_chat_endpoint_serves_strokes_field(client):
    r = client.post("/api/v1/chat", json={"text": "的", "stream": False, "mode": "dict"}, headers=ADMIN)
    assert r.status_code == 200
    assert r.json()["strokes"] == "1,2;3,4|5,6;7,8"

    # No stroke entry for this one: the card is still served, no field.
    r2 = client.post("/api/v1/chat", json={"text": "乐", "stream": False, "mode": "dict"}, headers=ADMIN)
    assert r2.status_code == 200
    assert "strokes" not in r2.json()
    # Not a single hanzi: no strokes either.
    r3 = client.post("/api/v1/chat", json={"text": "N", "stream": False, "mode": "dict"}, headers=ADMIN)
    assert "strokes" not in r3.json()


def test_describe_status(tmp_path):
    assert dict_source.describe("llm", "") == "llm"
    assert dict_source.describe("local", str(tmp_path)) == "local_missing_data"


def test_chat_endpoint_serves_local_card(client, monkeypatch):
    def _boom(settings):
        raise AssertionError("LLM must not be consulted for a local dict card")

    monkeypatch.setattr("relay.main.make_llm_provider", _boom)
    r = client.post("/api/v1/chat", json={"text": "的", "stream": False, "mode": "dict"}, headers=ADMIN)
    assert r.status_code == 200
    body = r.json()
    assert body["text"].startswith("【字】的\n【拼音】de、dì、dí\n")
    assert body["usage"] == {"inputTokens": 0, "outputTokens": 0}


def test_chat_endpoint_local_card_sse(client, monkeypatch):
    def _boom(settings):
        raise AssertionError("LLM must not be consulted for a local dict card")

    monkeypatch.setattr("relay.main.make_llm_provider", _boom)
    with client.stream(
        "POST", "/api/v1/chat", json={"text": "桛", "stream": True, "mode": "dict"}, headers=ADMIN
    ) as r:
        assert r.status_code == 200
        lines = [line for line in r.iter_lines() if line.startswith("data: ")]
    events = [json.loads(line[6:]) for line in lines]
    assert [e["type"] for e in events] == ["llm.start", "llm.delta", "session.done"]
    assert events[1]["text"].startswith("【字】桛\n")


def test_readyz_reports_dict_status(client):
    r = client.get("/readyz")
    assert r.json()["dict"] == "local"


REAL_DATA_DIR = Path(__file__).resolve().parents[1] / "dict-data"


@pytest.mark.skipif(not (REAL_DATA_DIR / "chars.json").is_file(), reason="dict-data not built")
def test_real_data_smoke():
    src = dict_source.DictSource(REAL_DATA_DIR)
    assert src.load()
    card = src.card("踌")
    assert card.startswith("【字】踌\n")
    assert "【部首笔画】足·14画" in card
    assert "踌躇" in card  # 组词
    assert len(card.encode("utf-8")) < 2048  # device REPLY_CAP
    assert "未收录" not in src.card("的")
    ke = src.card("科")
    assert "猫科" in ke and "～" not in ke  # headword placeholder substituted
    ext_a = src.card("㐂")  # Ext-A hanzi present in the data (kokuji)
    assert ext_a.startswith("【字】㐂\n") and "未收录" not in ext_a

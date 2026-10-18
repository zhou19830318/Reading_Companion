#!/usr/bin/env python3
"""Build the relay's offline dictionary card data.

Sources (all permissively licensed, see META.json):

  * Unihan (Unicode Consortium, Unicode License): kHanyuPinlu / kXHC1983 /
    kMandarin readings, kRSUnicode radical, kTotalStrokes.
  * chinese-xinhua (MIT, github.com/pwxcoo/chinese-xinhua): per-character
    `word.json` (modern concise text in the "郑码…"-terminated tail blocks)
    and `ci.json` (word list for 组词).
  * jieba dict.txt (MIT, github.com/fxsjy/jieba): corpus frequencies used to
    rank 组词 candidates so the card shows common words first.

Output (written to --out):

    chars.json   {char: [readings, radical, strokes, senses]}
    words.json   {char: [word, ...]}
    META.json    provenance, input hashes, counts, build time

Deterministic, stdlib only. Run once per data update and ship the directory
with the package (README: "Dictionary data"). The relay loads it at startup.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import unicodedata
import zipfile
from datetime import datetime, timezone
from pathlib import Path

# --------------------------------------------------------------------------
# Pinyin
# --------------------------------------------------------------------------

_TONE_MARKS = {
    "a": "āáǎà", "e": "ēéěè", "i": "īíǐì",
    "o": "ōóǒò", "u": "ūúǔù", "ü": "ǖǘǚǜ",
}
_READING_FIELDS = ("kHanyuPinlu", "kXHC1983", "kMandarin")


def normalize_pinyin(text: str) -> str:
    """Fold a raw Unihan reading into plain tone-marked pinyin.

    Handles the ɡ (U+0261) used in some sources, u: for ü, and tone digits
    (``hao3`` -> ``hǎo``); tone 0/5 is the neutral tone, left unmarked.
    """
    text = text.strip().replace("ɡ", "g").replace("u:", "ü").replace("v", "ü")
    m = re.fullmatch(r"([a-zü]+)([0-5])", text)
    if not m:
        return text.lower()
    base, tone = m.group(1), int(m.group(2))
    if tone in (0, 5):
        return base
    return _apply_tone(base, tone)


def _apply_tone(syllable: str, tone: int) -> str:
    """Place the tone mark per pinyin rules (a/e win, then last vowel)."""
    marks = _TONE_MARKS
    for vowel in "a", "e":
        if vowel in syllable:
            return syllable.replace(vowel, marks[vowel][tone - 1], 1)
    for combo, target in (("iu", "u"), ("ui", "i")):
        if combo in syllable:
            return syllable.replace(combo, combo[0] + marks[target][tone - 1], 1)
    for i in range(len(syllable) - 1, -1, -1):
        if syllable[i] in "aeiouü":
            return syllable[:i] + marks[syllable[i]][tone - 1] + syllable[i + 1:]
    return syllable


def parse_khanyupinlu(value: str) -> list[str]:
    """``de(75596) dì(157) dí(84)`` -> readings ordered by frequency."""
    out = []
    for chunk in value.split():
        m = re.match(r"^(.+?)\(\d+\)$", chunk)
        reading = normalize_pinyin(m.group(1) if m else chunk)
        if reading and reading not in out:
            out.append(reading)
    return out


def parse_reading_list(value: str) -> list[str]:
    """``0442.080:háng 0443.050:hàng`` or ``42644.160:dì,dí,de`` -> readings."""
    out = []
    for chunk in value.split():
        _, _, readings = chunk.partition(":")
        for reading in readings.split(","):
            reading = normalize_pinyin(reading)
            if reading and reading not in out:
                out.append(reading)
    return out


def pick_readings(fields: dict[str, str]) -> list[str]:
    """Best available reading list for one character, most common first."""
    if pinlu := parse_khanyupinlu(fields.get("kHanyuPinlu", "")):
        return pinlu[:4]
    if xhc := parse_reading_list(fields.get("kXHC1983", "")):
        return xhc[:4]
    mandarin = normalize_pinyin(fields.get("kMandarin", ""))
    return [mandarin] if mandarin else []


# --------------------------------------------------------------------------
# Radicals
# --------------------------------------------------------------------------

# Canonical Kangxi radical 1..214 order: NFKD-fold of the Unicode Kangxi
# Radicals block (U+2F00..U+2FD5). Deriving it here avoids transcription
# errors and stays correct as Python's unicodedata is updated.
KANGXI_RADICALS = "".join(unicodedata.normalize("NFKD", chr(0x2F00 + i)) for i in range(214))
assert len(KANGXI_RADICALS) == 214 and all(len(c) == 1 for c in KANGXI_RADICALS)

# Display overrides for a mainland reader: the simplified radical character
# where it is a standalone form (not a combining shape like 扌/氵).
_RADICAL_DISPLAY = {
    "戶": "户", "艸": "艹", "辵": "辶", "邑": "阝", "阜": "阝",
    "見": "见", "車": "车", "門": "门", "馬": "马", "魚": "鱼",
    "鳥": "鸟", "貝": "贝", "頁": "页", "風": "风", "長": "长",
    "韋": "韦", "飛": "飞", "麥": "麦", "黃": "黄", "齊": "齐",
    "齒": "齿", "龍": "龙", "龜": "龟", "鹵": "卤", "黽": "黾", "靑": "青",
}


def radical_for(rs_unicode: str) -> str:
    """kRSUnicode ``125.4`` / ``125'.4`` -> display radical character."""
    m = re.match(r"^(\d+)", rs_unicode.split()[0])
    if not m:
        return ""
    number = int(m.group(1))
    if not 1 <= number <= 214:
        return ""
    kangxi = KANGXI_RADICALS[number - 1]
    return _RADICAL_DISPLAY.get(kangxi, kangxi)


# --------------------------------------------------------------------------
# Senses (释义) — chinese-xinhua ``more`` concise blocks
# --------------------------------------------------------------------------

# Zhuyin + the tone marks the dataset uses: ˉ ˊ ˇ ˋ ˙ and the box-drawing
# glyph ╝ (U+255D) that stands in for the fourth tone (e.g. ㄉㄧ╝).
_ZHUYIN_LINE = re.compile(
    r"^([a-zɡ\u00e0-\u00ff\u0101-\u01ff]+)[ 　]+([\u3105-\u3129\u02C7\u02C9\u02CA\u02CB\u02D9\u255D]+)$"
)
_META_LINE = re.compile(r"^(郑码|笔画数|笔顺编号)")
_BRACKETED_EN = re.compile(r"[\[［][^\]］]*[\]］]")
_RULE_LINE = re.compile(r"^[─—-]+$")
_PAREN_VARIANT = re.compile(r"^（[^）]*）$")
_NUMBER_MARK = re.compile(r"^\(\d+\)$")
_POS_MARK = re.compile(r"^〈[^〉]*〉$")
_EN_GLOSS = re.compile(r"^[A-Za-z0-9 \.,;:!?'\"()\[\]-]+$")
_XREF_LINE = re.compile(r"^(另见|又)$")
_CITATION = re.compile(r"(--|《|又如)")


def extract_senses(char: str, more: str) -> list[tuple[str, list[str]]]:
    """Pull (reading, senses) blocks out of the ``more`` field.

    Only the modern concise blocks are used: a ``<char><digit?>`` header whose
    next line is ``pinyin␣zhuyin`` (this is what separates them from the full
    Kangxi-style sections, whose reading lines carry no zhuyin), ending at the
    ``郑码…`` meta lines. Senses are the ``(N)``-marked segments or, when
    unnumbered, the single definition line.
    """
    lines = [p.strip() for p in more.split("\n") if p.strip()]
    starts: list[tuple[int, str]] = []
    for i, line in enumerate(lines):
        if not re.fullmatch(re.escape(char) + r"\d?", line):
            continue
        j = i + 1
        if j < len(lines) and _PAREN_VARIANT.match(lines[j]):
            j += 1
        if j < len(lines) and (m := _ZHUYIN_LINE.match(lines[j])):
            starts.append((i, normalize_pinyin(m.group(1))))

    blocks: list[tuple[str, list[str]]] = []
    for idx, (_start, reading) in enumerate(starts):
        end = starts[idx + 1][0] if idx + 1 < len(starts) else len(lines)
        segment: list[str] = []
        for ln in lines[_start + 1 : end]:
            if _META_LINE.match(ln):
                break
            if _RULE_LINE.match(ln) or _POS_MARK.match(ln) or _XREF_LINE.match(ln):
                continue
            if _EN_GLOSS.match(ln) and re.search(r"[A-Za-z]", ln):
                continue
            if _ZHUYIN_LINE.match(ln):
                continue
            segment.append(ln)
        senses = _split_senses(segment)
        if senses:
            blocks.append((reading, senses))
    return blocks


_CHAR_READING_PREFIX = r"^%s[a-zɡ\u00e0-\u00ff\u0101-\u01ff]*$"


def senses_from_explanation(char: str, explanation: str, max_defs: int) -> list[str]:
    """Fallback for characters without a concise block.

    Two source layouts exist: ``{char}{reading} 1.sense 2.sense`` one-liners
    and newline-separated paragraphs (definition, citations, 又如 lists). The
    numbered one-liners are preferred; citations and the char+reading prefix
    are dropped either way.
    """
    prefix_only = re.compile(_CHAR_READING_PREFIX % re.escape(char))

    text = _BRACKETED_EN.sub("", explanation)
    if re.search(r"(?:^|\s)\d+\.", text):
        parts = [p.strip() for p in re.split(r"(?:^|\s)\d+\.\s*", text)]
        out = [p for p in parts if p and not prefix_only.match(p) and not _CITATION.search(p)]
        if out:
            return out[:max_defs]

    out = []
    for para in explanation.split("\n"):
        para = _BRACKETED_EN.sub("", para).strip()
        if not para or para == char or _CITATION.search(para):
            continue
        if re.match(re.escape(char) + r"[a-zɡ\u00e0-\u00ff\u0101-\u01ff]", para):
            continue
        out.append(para)
        if len(out) >= max_defs:
            break
    return out


def _split_senses(segment: list[str]) -> list[str]:
    if not segment:
        return []
    if any(_NUMBER_MARK.match(ln) for ln in segment):
        parts, current = [], None
        for ln in segment:
            if _NUMBER_MARK.match(ln):
                if current:
                    parts.append(current)
                current = []
                continue
            if current is not None:
                current.append(ln)
        if current:
            parts.append(current)
        return [_join_sense(p) for p in parts]
    return [_join_sense(segment)]


def _join_sense(lines: list[str]) -> str:
    """Join a definition line with its example lines.

    ``走`` + ``～走。步～。`` -> ``走：～走。步～。`` — a colon keeps the
    definition readable when the source glued them without punctuation.
    """
    cleaned = [_BRACKETED_EN.sub("", ln).strip() for ln in lines]
    cleaned = [ln for ln in cleaned if ln]
    if not cleaned:
        return ""
    head, rest = cleaned[0], cleaned[1:]
    if rest and head and head[-1] not in "。！？；：…":
        return head + "：" + "".join(rest)
    return head + "".join(rest)


_VARIANT_LABEL = re.compile(r"^（[^）]{1,4}）：")
_NUMBERED_LEAD = re.compile(r"^\d+\.\s*")


def _finalize_sense(sense: str, max_len: int) -> str:
    sense = _VARIANT_LABEL.sub("", sense)
    sense = _NUMBERED_LEAD.sub("", sense)
    sense = re.sub(r"\s+", "", sense)
    sense = sense.strip("、;；,，")
    if len(sense) > max_len:
        sense = sense[: max_len - 1].rstrip("，、。；：") + "…"
    return sense


def interleave_senses(
    blocks: list[tuple[str, list[str]]], primary_reading: str, max_defs: int, max_len: int
) -> list[str]:
    """Up to ``max_defs`` senses. The block matching kHanyuPinlu's most
    frequent reading comes first, then one sense per other block, then back to
    the top block for extras. Reading prefixes appear only for polyphones."""
    if not blocks:
        return []
    ordered = sorted(blocks, key=lambda b: 0 if b[0] == primary_reading else 1)
    readings = [reading for reading, _ in ordered]
    prefix = len(ordered) > 1 and all(readings)
    queues = [[_finalize_sense(s, max_len) for s in senses] for _, senses in ordered]
    out: list[str] = []
    while len(out) < max_defs and any(queues):
        for i, queue in enumerate(queues):
            if not queue:
                continue
            sense = queue.pop(0)
            if not sense:
                continue
            text = f"{readings[i]}：{sense}" if prefix else sense
            if text not in out:
                out.append(text)
            if len(out) >= max_defs:
                break
    return out


# --------------------------------------------------------------------------
# 组词 words
# --------------------------------------------------------------------------

_CJK = re.compile(r"^[\u3400-\u4dbf\u4e00-\u9fff]+$")

# Curse/vulgar compounds that are corpus-frequent but have no place on a
# reading device's dictionary card.
_BLOCK_WORDS = {
    "他妈的", "他妈", "妈的", "你妈的", "去你妈的", "操他妈", "日你妈",
    "狗日的", "王八蛋", "龟儿子", "滚他妈", "妈的巴子", "马了个逼", "傻逼",
}


def _is_word(word: str) -> bool:
    return 2 <= len(word) <= 4 and _CJK.match(word) is not None and word not in _BLOCK_WORDS


def build_word_index(jieba_path: Path | None, ci_words: list[str]) -> dict[str, list[str]]:
    """char -> ranked words.

    Corpus words (jieba, frequency-ranked) come first, but words that also
    appear in the xinhua 词语 list rank above corpus-only ones — jieba's
    segmentation corpus contains junk like ``公凸`` that the word list does
    not. The remaining xinhua words fill in afterwards (short words first).
    """
    ci_set = {word for word in ci_words if _is_word(word)}

    ranked: dict[str, list[tuple[bool, int, str]]] = {}
    if jieba_path is not None:
        with open(jieba_path, encoding="utf-8") as fh:
            for line in fh:
                parts = line.split()
                if len(parts) < 2 or not _is_word(parts[0]):
                    continue
                try:
                    freq = int(parts[1])
                except ValueError:
                    continue
                for ch in set(parts[0]):
                    ranked.setdefault(ch, []).append((parts[0] not in ci_set, -freq, parts[0]))

    index: dict[str, list[str]] = {}
    for ch, entries in ranked.items():
        entries.sort()
        words: list[str] = []
        for _, _, word in entries:
            if word not in words:
                words.append(word)
        index[ch] = words

    ci_index: dict[str, list[str]] = {}
    for word in ci_set:
        for ch in set(word):
            ci_index.setdefault(ch, []).append(word)
    for ch, words in ci_index.items():
        current = index.setdefault(ch, [])
        for word in sorted(set(words), key=lambda w: (len(w), w)):
            if word not in current:
                current.append(word)
    return index


# --------------------------------------------------------------------------
# Sources and build
# --------------------------------------------------------------------------

def _iter_unihan(path: Path):
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t", 2)
            if len(parts) == 3:
                yield parts[0], parts[1], parts[2]


def read_unihan(unihan_dir: Path) -> tuple[dict[str, dict[str, str]], dict[str, str], dict[str, str]]:
    """(cp -> {reading field: value}, cp -> kRSUnicode, cp -> kTotalStrokes).

    kRSUnicode and kTotalStrokes live in Unihan_IRGSources.txt; the readings
    fields in Unihan_Readings.txt (Unihan 15.1 layout).
    """
    readings: dict[str, dict[str, str]] = {}
    radicals: dict[str, str] = {}
    strokes: dict[str, str] = {}

    for cp, field, value in _iter_unihan(unihan_dir / "Unihan_Readings.txt"):
        if field in _READING_FIELDS:
            readings.setdefault(cp, {})[field] = value
    for cp, field, value in _iter_unihan(unihan_dir / "Unihan_IRGSources.txt"):
        if field == "kRSUnicode":
            radicals[cp] = value
        elif field == "kTotalStrokes":
            strokes[cp] = value
    return readings, radicals, strokes


def read_xinhua(word_path: Path) -> dict[str, list[dict]]:
    entries = json.loads(word_path.read_text(encoding="utf-8"))
    by_char: dict[str, list[dict]] = {}
    for entry in entries:
        word = entry["word"]
        if len(word) != 1:
            continue
        by_char.setdefault(word, []).append(entry)
    return by_char


def select_entries(entries: list[dict], readings: list[str], strokes: int) -> list[dict]:
    """Disambiguate characters with duplicate xinhua rows.

    1332 characters have a second, mismatched row in the source scrape (wrong
    stroke count / reading, generic "日用汉字" filler text). Keep only rows
    whose stroke count matches Unihan; if none does, whose pinyin matches a
    Unihan reading; if still none, keep everything.
    """
    if len(entries) < 2:
        return entries
    by_strokes = [e for e in entries if strokes and str(e.get("strokes", "")).strip() == str(strokes)]
    if by_strokes:
        return by_strokes
    by_reading = [e for e in entries if normalize_pinyin(str(e.get("pinyin", ""))) in readings]
    return by_reading or entries


def merge_blocks(entries: list[dict], max_fallback: int = 2) -> list[tuple[str, list[str]]]:
    """All sense blocks across a character's entries, deduplicated in order.

    When no concise block exists (about half the rare characters), fall back
    to the first non-citation paragraphs of the explanation field.
    """
    merged: list[tuple[str, list[str]]] = []
    seen: set[tuple[str, str]] = set()
    for entry in entries:
        for reading, senses in extract_senses(entry["word"], entry.get("more", "")):
            key = (reading, senses[0] if senses else "")
            if key in seen:
                continue
            seen.add(key)
            merged.append((reading, senses))
    if not merged:
        for entry in entries:
            senses = senses_from_explanation(entry["word"], entry.get("explanation", ""), max_fallback)
            if senses:
                merged.append(("", senses))
                break
    return merged


def build(
    word_path: Path,
    ci_path: Path | None,
    unihan_dir: Path,
    jieba_path: Path | None,
    top_words: int,
    max_defs: int,
    max_def_len: int,
) -> tuple[dict[str, list], dict[str, list[str]]]:
    by_char = read_xinhua(word_path)
    readings, radicals, strokes = read_unihan(unihan_dir)

    chars: dict[str, list] = {}
    codepoints = {f"U+{ord(ch):04X}" for ch in by_char} | set(radicals)
    for cp in sorted(codepoints):
        try:
            ch = chr(int(cp[2:], 16))
        except ValueError:
            continue
        if not ("\u3400" <= ch <= "\u9fff" or "\uf900" <= ch <= "\ufaff"):
            continue
        char_readings = pick_readings(readings.get(cp, {}))
        stroke_count = strokes.get(cp, "").split()[0] if cp in strokes else ""
        strokes_int = int(stroke_count) if stroke_count.isdigit() else 0
        entries = select_entries(by_char.get(ch, []), char_readings, strokes_int)
        blocks = merge_blocks(entries)
        senses = interleave_senses(blocks, char_readings[0] if char_readings else "", max_defs, max_def_len)
        radical = radical_for(radicals.get(cp, ""))
        if not char_readings and not senses and not radical:
            continue
        chars[ch] = [char_readings, radical, strokes_int, senses]

    ci_words = []
    if ci_path is not None and ci_path.exists():
        ci_words = [e["ci"] for e in json.loads(ci_path.read_text(encoding="utf-8"))]
    word_index = build_word_index(jieba_path, ci_words)
    words = {ch: index[:top_words] for ch, index in word_index.items() if ch in chars and index}
    return chars, words


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--word", type=Path, required=True, help="chinese-xinhua data/word.json")
    parser.add_argument("--ci", type=Path, default=None, help="chinese-xinhua data/ci.json (optional)")
    parser.add_argument("--unihan", type=Path, required=True, help="Unihan directory or Unihan.zip")
    parser.add_argument("--jieba", type=Path, default=None, help="jieba dict.txt (optional, ranking)")
    parser.add_argument("--out", type=Path, required=True, help="output directory (relay/dict-data)")
    parser.add_argument("--top-words", type=int, default=5)
    parser.add_argument("--max-defs", type=int, default=3)
    parser.add_argument("--max-def-len", type=int, default=64)
    args = parser.parse_args()

    unihan_dir = args.unihan
    if unihan_dir.suffix == ".zip":
        unihan_dir = unihan_dir.with_suffix("")
        with zipfile.ZipFile(args.unihan) as zf:
            zf.extractall(unihan_dir)

    assert len(KANGXI_RADICALS) == 214, "radical table corrupted"
    chars, words = build(
        args.word, args.ci, unihan_dir, args.jieba, args.top_words, args.max_defs, args.max_def_len
    )

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "chars.json").write_text(json.dumps(chars, ensure_ascii=False, sort_keys=True), encoding="utf-8")
    (args.out / "words.json").write_text(json.dumps(words, ensure_ascii=False, sort_keys=True), encoding="utf-8")
    sources = {"word.json": sha256(args.word)}
    if args.ci and args.ci.exists():
        sources["ci.json"] = sha256(args.ci)
    if args.jieba and args.jieba.exists():
        sources["jieba_dict.txt"] = sha256(args.jieba)
    if args.unihan.suffix == ".zip":
        sources["Unihan.zip"] = sha256(args.unihan)
    meta = {
        "built_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "chars": len(chars),
        "with_senses": sum(1 for v in chars.values() if v[3]),
        "with_words": len(words),
        "sources": sources,
        "licenses": {"Unihan": "Unicode License (permissive)", "chinese-xinhua": "MIT", "jieba": "MIT"},
    }
    (args.out / "META.json").write_text(
        json.dumps(meta, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"chars={len(chars)} with_senses={meta['with_senses']} with_words={meta['with_words']}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

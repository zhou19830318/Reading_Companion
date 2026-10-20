"""Offline dictionary cards for /api/v1/chat mode="dict".

The card facts come from the pre-built data directory (see
tools/build_dict_data.py): Unihan readings / radical / stroke count plus
chinese-xinhua senses and 组词, so the device never receives generated
dictionary content. A character that is not in the data gets an explicit
"not in the offline dictionary" card instead of an LLM guess.

Strokes: strokes.json (tools/build_stroke_data.py, Make Me a Hanzi medians)
holds one pre-encoded stroke-order payload per character for the device's
田字格 stroke view. It is optional: without it the card service is unchanged
and the device simply offers no stroke view.

Configuration: DICT_SOURCE=local (default, this module) or "llm" (the old
LLM-generated cards). Data directory resolution: DICT_DATA_DIR, then
$RELEASE/dict-data next to the relay package, then relay/dict-data in a
development checkout.
"""

from __future__ import annotations

import json
import logging
import re
from pathlib import Path

log = logging.getLogger("relay.dict_source")

NOT_A_CHAR_SENSE = "请输入单个汉字。"
NOT_FOUND_SENSE = "该字未收录于离线字典。"
UNRECOGNIZED_HERO = "无法识别"

# Same ranges the build tool accepts (BMP + Ext-A). A letter or symbol from
# the page picker is "not a hanzi", not "missing from the dictionary".
_CJK_CHAR = re.compile(r"^[\u3400-\u4dbf\u4e00-\u9fff]$")


def candidate_data_dirs() -> list[Path]:
    here = Path(__file__).resolve()
    return [
        here.parents[2] / "dict-data",  # release root: $RELEASE/dict-data
        here.parents[1] / "dict-data",  # dev checkout: relay/dict-data
    ]


def resolve_data_dir(configured: str) -> Path | None:
    """The dict-data directory, or None.

    An explicitly configured DICT_DATA_DIR must contain chars.json — a typo
    should surface as "missing data", not silently pick up a checkout copy.
    """
    if configured:
        path = Path(configured)
        return path if (path / "chars.json").is_file() else None
    for path in candidate_data_dirs():
        if (path / "chars.json").is_file():
            return path
    return None


def describe(configured_source: str, configured_dir: str) -> str:
    """Status string for /readyz: llm | local | local_missing_data."""
    if configured_source == "llm":
        return "llm"
    return "local" if resolve_data_dir(configured_dir) is not None else "local_missing_data"


def render_card(
    char: str,
    readings: list[str],
    radical: str,
    strokes: int,
    senses: list[str],
    words: list[str],
) -> str:
    """The 【...】-line card the device's DictionaryActivity parses.

    Empty sections are omitted; the device draws 【字】 as the hero glyph.
    """
    lines = [f"【字】{char}"]
    if readings:
        lines.append(f"【拼音】{'、'.join(readings)}")
    if radical and strokes:
        lines.append(f"【部首笔画】{radical}·{strokes}画")
    elif radical:
        lines.append(f"【部首笔画】{radical}")
    if senses:
        # Xinhua writes the headword as "～" inside definitions ("猫～" = 猫科).
        substituted = [s.replace("～", char) for s in senses]
        lines.append(f"【释义】{' '.join(substituted)}")
    if words:
        lines.append(f"【组词】{'、'.join(words)}")
    return "\n".join(lines) + "\n"


class DictSource:
    """Read-only view of one dict-data directory."""

    def __init__(self, data_dir: Path | str) -> None:
        self.data_dir = Path(data_dir)
        self.chars: dict[str, list] = {}
        self.words: dict[str, list[str]] = {}
        self.strokes: dict[str, str] = {}
        self.meta: dict = {}
        self.loaded = False

    def load(self) -> bool:
        try:
            self.chars = json.loads((self.data_dir / "chars.json").read_text(encoding="utf-8"))
            words_path = self.data_dir / "words.json"
            if words_path.is_file():
                self.words = json.loads(words_path.read_text(encoding="utf-8"))
            strokes_path = self.data_dir / "strokes.json"
            if strokes_path.is_file():
                self.strokes = json.loads(strokes_path.read_text(encoding="utf-8"))
            meta_path = self.data_dir / "META.json"
            if meta_path.is_file():
                self.meta = json.loads(meta_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            log.error("dict data load failed", extra={"dir": str(self.data_dir), "error": str(exc)})
            return False
        self.loaded = True
        log.info(
            "dict data loaded",
            extra={
                "dir": str(self.data_dir),
                "chars": len(self.chars),
                "strokes": len(self.strokes),
                "built_at": self.meta.get("built_at", ""),
            },
        )
        return True

    def card(self, text: str) -> str:
        """Always returns a card: entries, not-found, or not-a-single-char."""
        char = text.strip()
        if len(char) != 1 or not _CJK_CHAR.match(char):
            return render_card(UNRECOGNIZED_HERO, [], "", 0, [NOT_A_CHAR_SENSE], [])
        entry = self.chars.get(char)
        if entry is None:
            return render_card(char, [], "", 0, [NOT_FOUND_SENSE], [])
        readings, radical, strokes, senses = entry
        return render_card(char, readings, radical, strokes, senses, self.words.get(char, []))

    def stroke_payload(self, text: str) -> str | None:
        """The character's encoded stroke-order payload, or None.

        Independent of the card entry: a character missing from chars.json
        but present in strokes.json still gets the stroke view.
        """
        char = text.strip()
        if len(char) != 1 or not _CJK_CHAR.match(char):
            return None
        return self.strokes.get(char)


_source: DictSource | None = None
_missing_logged = False


def get_dict_source(configured_dir: str) -> DictSource | None:
    """Load (once) and return the shared source, or None when unavailable."""
    global _source, _missing_logged
    if _source is not None:
        return _source
    data_dir = resolve_data_dir(configured_dir)
    if data_dir is None:
        if not _missing_logged:  # one error, not one per lookup
            _missing_logged = True
            log.error("DICT_SOURCE=local but no dict-data found; falling back to LLM cards")
        return None
    source = DictSource(data_dir)
    if not source.load():
        return None
    _source = source
    return _source


def reset_cache() -> None:
    """Test hook: drop the loaded data so the next call re-resolves."""
    global _source, _missing_logged
    _source = None
    _missing_logged = False

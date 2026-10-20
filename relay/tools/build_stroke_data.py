#!/usr/bin/env python3
"""Build the relay's stroke-order data (for the 田字格 stroke view).

Source (permissively licensed, see STROKES_META.json):

  * Make Me a Hanzi graphics.txt (github.com/skishore/makemeahanzi), derived
    from the Arphic PL KaitiM GB / UKai fonts: per-character stroke medians —
    one polyline per stroke, in a fixed y-up canvas (x 0..1024, baseline y=0,
    ascender ~880, descender ~-144). The data ships under the Arphic Public
    License (redistribution allowed, license text included in the metadata).

Output (written to --out, alongside chars.json):

    strokes.json       {char: "x,y;x,y|..."}   one encoded entry per character
    STROKES_META.json  provenance, counts, frame, build time

Encoding: coordinates are quantized into the fixed frame below to 0..255
(x 0..1024, y flipped to screen order -144..880), so every entry is portable:
the device maps each coordinate to its own 田字格 with ``pos * (size-1) / 255``.
Points in a stroke are separated by ``;``, strokes by ``|``. Strokes with a
single median point cannot be drawn and are dropped.

The relay serves one entry verbatim inside the dict card response's "strokes"
field (max encoded length is capped by the device buffer; STROKES_META records
the observed maximum so the device constant can be re-checked after a data
update).

Deterministic, stdlib only. Run once per data update and ship the directory
with the package (README: "Dictionary data").
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

# The quantization frame, in makemeahanzi's y-up canvas: the ideographic
# square is x 0..1024, y -144..880 (0 = baseline, ~880 = ascender, the small
# descent below the baseline is kept so a handful of descenders fit).
FRAME_X0 = 0.0
FRAME_X1 = 1024.0
FRAME_Y_BOTTOM = -144.0
FRAME_Y_TOP = 880.0

SOURCE_URL = "https://raw.githubusercontent.com/skishore/makemeahanzi/master/graphics.txt"


def quantize(x: float, y: float) -> tuple[int, int]:
    """Canvas coordinate -> 0..255, y flipped to screen order (0 = top)."""
    nx = round((x - FRAME_X0) / (FRAME_X1 - FRAME_X0) * 255)
    ny = round((FRAME_Y_TOP - y) / (FRAME_Y_TOP - FRAME_Y_BOTTOM) * 255)
    return max(0, min(255, nx)), max(0, min(255, ny))


def encode_medians(medians: list) -> str:
    """One character's medians -> "x,y;x,y|..." ("" when nothing is drawable)."""
    strokes = []
    for median in medians:
        if len(median) < 2:  # a single point cannot be drawn as a stroke
            continue
        points = []
        for point in median:
            nx, ny = quantize(point[0], point[1])
            points.append(f"{nx},{ny}")
        strokes.append(";".join(points))
    return "|".join(strokes)


def build(graphics_path: Path) -> tuple[dict[str, str], dict]:
    """graphics.txt (one JSON object per line) -> {char: encoded}, stats.

    Characters with no drawable stroke are skipped rather than shipped empty;
    the device shows the stroke view only for characters that have an entry.
    """
    strokes: dict[str, str] = {}
    skipped = 0
    max_encoded = 0
    max_encoded_char = ""
    max_strokes = 0
    max_points = 0
    with open(graphics_path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            entry = json.loads(line)
            encoded = encode_medians(entry.get("medians", []))
            if not encoded:
                skipped += 1
                continue
            character = entry["character"]
            strokes[character] = encoded
            if len(encoded) > max_encoded:
                max_encoded, max_encoded_char = len(encoded), character
            max_strokes = max(max_strokes, encoded.count("|") + 1)
            max_points = max(max_points, encoded.count(";") + encoded.count("|") + 1)
    stats = {
        "chars": len(strokes),
        "skipped": skipped,
        "max_encoded_len": max_encoded,
        "max_encoded_char": max_encoded_char,
        "max_strokes": max_strokes,
        "max_points": max_points,
    }
    return strokes, stats


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--graphics", type=Path, required=True, help="makemeahanzi graphics.txt")
    parser.add_argument("--out", type=Path, required=True, help="output directory (relay/dict-data)")
    args = parser.parse_args()

    strokes, stats = build(args.graphics)

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "strokes.json").write_text(
        json.dumps(strokes, ensure_ascii=False, sort_keys=True), encoding="utf-8"
    )
    meta = {
        "built_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "source_url": SOURCE_URL,
        "sources": {"graphics.txt": sha256(args.graphics)},
        "frame": {
            "x": [FRAME_X0, FRAME_X1],
            "y": [FRAME_Y_BOTTOM, FRAME_Y_TOP],
            "quantized": "0..255, y screen-order (0 = top)",
        },
        "licenses": {
            "Make Me a Hanzi graphics.txt": "Arphic Public License (derived from Arphic PL KaitiM GB / UKai)"
        },
        **stats,
    }
    (args.out / "STROKES_META.json").write_text(
        json.dumps(meta, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        f"chars={stats['chars']} skipped={stats['skipped']} "
        f"max_encoded_len={stats['max_encoded_len']} ({stats['max_encoded_char']}) "
        f"max_strokes={stats['max_strokes']}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

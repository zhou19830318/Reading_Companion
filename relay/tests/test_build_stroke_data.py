"""Stroke-order data build tool: quantization, encoding, and the map build."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

# tools/ is not a package; the build scripts are imported by path.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import build_stroke_data as bsd  # noqa: E402

DICT_DATA = Path(__file__).resolve().parents[1] / "dict-data"


def test_quantize_corners_and_clamp():
    assert bsd.quantize(0.0, 880.0) == (0, 0)  # frame top-left -> screen top-left
    assert bsd.quantize(1024.0, -144.0) == (255, 255)  # frame bottom-right
    assert bsd.quantize(-50.0, 2000.0) == (0, 0)  # clamped into the frame
    assert bsd.quantize(5000.0, -5000.0) == (255, 255)
    # Interior point: x 256/1024 and y (880-624)/1024 both land on 63.75 -> 64.
    assert bsd.quantize(256.0, 624.0) == (64, 64)


def test_encode_medians_exact_string():
    medians = [[[256.0, 624.0], [768.0, 112.0]]]
    assert bsd.encode_medians(medians) == "64,64;191,191"


def test_encode_medians_drops_undrawable_strokes():
    medians = [[[100, 100]], [[0, 880], [1024, -144]]]  # first stroke: one point
    assert bsd.encode_medians(medians) == "0,0;255,255"
    assert bsd.encode_medians([]) == ""
    assert bsd.encode_medians([[[1, 2]]]) == ""


def test_build_from_graphics_file(tmp_path):
    lines = [
        json.dumps({"character": "人", "medians": [[[0, 880], [1024, -144]]]}),
        json.dumps({"character": "一", "medians": [[[0, 0]]]}),  # undrawable -> skipped
    ]
    path = tmp_path / "graphics.txt"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    strokes, stats = bsd.build(path)
    assert strokes == {"人": "0,0;255,255"}
    assert stats["chars"] == 1
    assert stats["skipped"] == 1
    assert stats["max_encoded_len"] == len("0,0;255,255")
    assert stats["max_strokes"] == 1


@pytest.mark.skipif(not (DICT_DATA / "strokes.json").is_file(), reason="no built stroke data in this checkout")
def test_real_data_shape():
    data = json.loads((DICT_DATA / "strokes.json").read_text(encoding="utf-8"))
    assert len(data) > 9000
    assert "人" in data
    # Every coordinate must be inside the quantized frame...
    for stroke in data["人"].split("|"):
        for point in stroke.split(";"):
            nx, ny = (int(v) for v in point.split(","))
            assert 0 <= nx <= 255
            assert 0 <= ny <= 255
    # ...and no entry may exceed the device's stroke buffer
    # (DictionaryActivity::STROKES_CAP).
    assert max(len(v) for v in data.values()) <= 1152

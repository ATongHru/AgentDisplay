#!/usr/bin/env python3
"""Generate LVGL CJK font: ASCII 0x20-0x7F + 通用规范汉字表一级字 3500."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHARS_FILE = ROOT / "third_party" / "fonts" / "gsc_level1_chars.txt"
OUT_C = ROOT / "main" / "font_cjk_16.c"
OUT_H = ROOT / "main" / "font_cjk_16.h"
FONT_CANDIDATES = [
    Path("C:/Windows/Fonts/simhei.ttf"),
    Path("C:/Windows/Fonts/Deng.ttf"),
    Path("C:/Windows/Fonts/msyh.ttc"),
    Path("C:/Windows/Fonts/simsun.ttc"),
]

# Chinese punctuation not in 一级字表, needed for UI/voice text.
EXTRA_SYMBOLS = "。，、：；？！「」『』（）【】《》…—·￥"


def load_level1() -> str:
    text = CHARS_FILE.read_text(encoding="utf-8")
    chars = []
    seen = set()
    for ch in text:
        if ch.isspace():
            continue
        if ch not in seen:
            seen.add(ch)
            chars.append(ch)
    if len(chars) != 3500:
        raise SystemExit(f"expected 3500 level-1 chars, got {len(chars)}")
    return "".join(chars)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--size", type=int, default=16)
    parser.add_argument("--bpp", type=int, default=4)
    parser.add_argument("--font", type=Path, default=None)
    args = parser.parse_args()

    font_path = args.font or next((p for p in FONT_CANDIDATES if p.exists()), None)
    if font_path is None:
        print("Font not found:", ", ".join(str(p) for p in FONT_CANDIDATES), file=sys.stderr)
        return 1

    level1 = load_level1()
    extra = "".join(dict.fromkeys(ch for ch in EXTRA_SYMBOLS if ch not in level1))
    symbols = extra + level1

    npx = shutil.which("npx") or shutil.which("npx.cmd")
    if not npx:
        print("npx not found", file=sys.stderr)
        return 1

    cmd = [
        npx,
        "--yes",
        "lv_font_conv",
        "--size",
        str(args.size),
        "--bpp",
        str(args.bpp),
        "--format",
        "lvgl",
        "--no-compress",
        "--no-prefilter",
        "--no-kerning",
        "--font",
        str(font_path),
        "-r",
        "0x20-0x7F",
        "--symbols",
        symbols,
        "--lv-font-name",
        "font_cjk_16",
        "--lv-include",
        "lvgl.h",
        "-o",
        str(OUT_C),
    ]
    print(f"font={font_path}")
    print(f"size={args.size} bpp={args.bpp} level1={len(level1)} extra={len(extra)} ascii=0x20-0x7F")
    print("running lv_font_conv ...")
    subprocess.run(cmd, check=True)

    size = OUT_C.stat().st_size
    OUT_H.write_text(
        "#pragma once\n\n"
        "#include \"lvgl.h\"\n\n"
        "extern const lv_font_t font_cjk_16;\n",
        encoding="utf-8",
    )
    print(f"wrote {OUT_C} ({size} bytes)")
    print(f"wrote {OUT_H}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

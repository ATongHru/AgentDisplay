#!/usr/bin/env python3
"""Flash font_cjk_16.bin to the cjk_font partition (offset 0x400000)."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "firmware" / "data" / "font_cjk_16.bin"
PARTITION_OFFSET = 0x400000


def find_python() -> str:
    candidates = [
        os.environ.get("IDF_PYTHON_ENV_PATH", ""),
        r"D:\Espressif\tools\python\v5.4.2\venv",
    ]
    for base in candidates:
        if not base:
            continue
        for name in ("python.exe", "python"):
            py = Path(base) / "Scripts" / name
            if py.is_file():
                return str(py)
            py = Path(base) / "bin" / name
            if py.is_file():
                return str(py)
    return sys.executable


def find_esptool() -> list[str]:
    python = find_python()
    idf_candidates = [
        os.environ.get("IDF_PATH", ""),
        r"D:\Espressif\.espressif\v5.4.2\esp-idf",
    ]
    for idf_path in idf_candidates:
        if not idf_path:
            continue
        idf_esptool = Path(idf_path) / "components" / "esptool_py" / "esptool" / "esptool.py"
        if idf_esptool.is_file():
            return [python, str(idf_esptool)]

    which = shutil.which("esptool.py")
    if which:
        return [which]

    which = shutil.which("esptool")
    if which:
        return [which]

    return [python, "-m", "esptool"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--port", default="COM5")
    parser.add_argument("--baud", default="921600")
    args = parser.parse_args()

    if not BIN.exists():
        print(f"Missing {BIN}. Run: python scripts/gen_cjk_font.py", file=sys.stderr)
        return 1

    esptool = find_esptool()
    cmd = [
        *esptool,
        "--chip",
        "esp32s3",
        "--port",
        args.port,
        "--baud",
        str(args.baud),
        "write_flash",
        hex(PARTITION_OFFSET),
        str(BIN),
    ]
    subprocess.run(cmd, check=True)
    print(f"Flashed {BIN.name} ({BIN.stat().st_size} bytes) at {hex(PARTITION_OFFSET)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

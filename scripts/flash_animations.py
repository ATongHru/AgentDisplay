#!/usr/bin/env python3
"""Flash animations.bin to the animations partition (offset 0x600000)."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "firmware" / "data" / "animations.bin"
PARTITION_OFFSET = 0x600000


def find_esptool() -> list[str]:
    """Prefer IDF/python esptool; do not use Arduino esptool.exe paths."""
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        idf_esptool = Path(idf_path) / "components" / "esptool_py" / "esptool" / "esptool.py"
        if idf_esptool.is_file():
            return [sys.executable, str(idf_esptool)]

    which = shutil.which("esptool.py")
    if which:
        return [which]

    which = shutil.which("esptool")
    if which:
        return [which]

    return [sys.executable, "-m", "esptool"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--port", default="COM4")
    parser.add_argument("--baud", default="921600")
    args = parser.parse_args()

    if not BIN.exists():
        print(f"Missing {BIN}. Run: python scripts/gen_frame_player.py", file=sys.stderr)
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

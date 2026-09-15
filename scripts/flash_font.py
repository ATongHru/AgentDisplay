#!/usr/bin/env python3
"""Flash font_cjk_16.bin to the cjk_font partition (offset 0x610000)."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from flash_util import find_esptool, require_port

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "firmware" / "data" / "font_cjk_16.bin"
PARTITION_OFFSET = 0x610000


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--port", default=None, help="Serial port (or set ESPPORT)")
    parser.add_argument("--baud", default="921600")
    args = parser.parse_args()

    if not BIN.exists():
        print(f"Missing {BIN}. Run: python scripts/gen_cjk_font.py", file=sys.stderr)
        return 1

    port = require_port(args.port)
    esptool = find_esptool()
    cmd = [
        *esptool,
        "--chip",
        "esp32s3",
        "--port",
        port,
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

"""Shared helpers for ESP-IDF flash scripts."""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path


def find_python() -> str:
    env_py = os.environ.get("IDF_PYTHON_ENV_PATH", "").strip()
    candidates: list[Path] = []
    if env_py:
        candidates.extend(
            [
                Path(env_py) / "Scripts" / "python.exe",
                Path(env_py) / "Scripts" / "python",
                Path(env_py) / "bin" / "python",
            ]
        )
    which = shutil.which("python") or shutil.which("python3")
    if which:
        candidates.append(Path(which))
    for py in candidates:
        if py.is_file():
            return str(py)
    return sys.executable


def find_esptool() -> list[str]:
    python = find_python()
    idf_path = os.environ.get("IDF_PATH", "").strip()
    if idf_path:
        idf_esptool = Path(idf_path) / "components" / "esptool_py" / "esptool" / "esptool.py"
        if idf_esptool.is_file():
            return [python, str(idf_esptool)]

    which = shutil.which("esptool.py") or shutil.which("esptool")
    if which:
        return [which]

    return [python, "-m", "esptool"]


def require_port(port: str | None) -> str:
    value = (port or os.environ.get("ESPPORT") or "").strip()
    if not value:
        raise SystemExit("Serial port required: pass -p COMx or set ESPPORT")
    return value

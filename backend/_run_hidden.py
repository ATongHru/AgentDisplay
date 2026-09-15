"""Launch uvicorn without a visible console (pythonw + start /B)."""

from __future__ import annotations

import os
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent
os.chdir(ROOT)

log_path = ROOT / "backend.log"
log_f = open(log_path, "a", encoding="utf-8", buffering=1)
sys.stdout = log_f
sys.stderr = log_f
print(f"\n--- start {datetime.now().isoformat(timespec='seconds')} ---", flush=True)

import os

import uvicorn

try:
    from dotenv import load_dotenv

    load_dotenv(ROOT / ".env")
except ImportError:
    pass

host = os.getenv("AGENT_BIND_HOST", "127.0.0.1").strip() or "127.0.0.1"
port = int(os.getenv("AGENT_BIND_PORT", "8000"))

uvicorn.run(
    "main:app",
    host=host,
    port=port,
    ws_ping_interval=None,
    ws_ping_timeout=None,
)

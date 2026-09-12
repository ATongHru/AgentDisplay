"""Capture Python stdout/stderr for the dashboard log panel."""

from __future__ import annotations

import sys
import threading
from collections import deque
from datetime import datetime
from typing import Callable

_SKIP_SUBSTR = (
    "GET /api/status",
    "GET /api/logs",
    "GET /health",
    "GET /favicon",
)

_WS_NOISE = (
    "keepalive ping timeout",
    "ConnectionClosedError exception in shielded future",
    "exception in shielded future",
    "no close frame received",
    "data transfer failed",
    "WinError 121",
    "信号灯超时时间已到",
    "websockets.exceptions.ConnectionClosedError",
    "websockets.legacy.",
    "asyncio.exceptions.CancelledError",
    "The above exception was the direct cause",
    "VoskAPI",
    "LOG (Vosk",
    "WARNING (Vosk",
    "ERROR (Vosk",
)

Listener = Callable[[dict], None]


def _guess_level(text: str) -> str:
    low = text.lower()
    if any(token in low for token in ("error", "failed", "traceback", "exception")):
        return "error"
    if "warn" in low:
        return "warn"
    return "info"


def _is_ws_noise(text: str) -> bool:
    if any(token in text for token in _WS_NOISE):
        return True
    stripped = text.lstrip()
    if stripped.startswith("future: <Future finished exception="):
        return True
    if "websockets" in text and ("File " in text or text.startswith("  ")):
        return True
    if any(name in text for name in ("asyncio\\streams", "asyncio/streams", "proactor_events", "windows_events", "asyncio\\windows")):
        return True
    return False


class LogHub:
    def __init__(self, maxlen: int = 300):
        self._items: deque[dict] = deque(maxlen=maxlen)
        self._lock = threading.Lock()
        self._listener: Listener | None = None
        self._emitting = threading.local()
        self._skip_traceback = False

    def set_listener(self, fn: Listener | None) -> None:
        self._listener = fn

    def items(self) -> list[dict]:
        with self._lock:
            return list(self._items)

    def emit(self, text: str, level: str | None = None) -> None:
        if getattr(self._emitting, "on", False):
            return
        text = (text or "").rstrip()
        if not text:
            return
        if any(token in text for token in _SKIP_SUBSTR):
            return
        if _is_ws_noise(text):
            self._skip_traceback = True
            return
        if self._skip_traceback:
            stripped = text.lstrip()
            if (
                not stripped
                or stripped.startswith("File ")
                or stripped.startswith("Traceback")
                or stripped.startswith("The above")
                or stripped.startswith("future:")
                or text.startswith(" ")
                or text.startswith("^")
                or stripped.endswith("Error")
                or "Error:" in stripped[:80]
            ):
                return
            self._skip_traceback = False
        item = {
            "time": datetime.now().strftime("%H:%M:%S"),
            "level": level or _guess_level(text),
            "text": text[:2000],
        }
        with self._lock:
            self._items.appendleft(item)
        listener = self._listener
        if listener is None:
            return
        self._emitting.on = True
        try:
            listener(item)
        except Exception:
            pass
        finally:
            self._emitting.on = False


class _Tee:
    def __init__(self, stream):
        self._stream = stream
        self._buf = ""

    def write(self, data: str) -> int:
        if not isinstance(data, str):
            data = str(data)
        if not data:
            return 0
        try:
            self._stream.write(data)
        except Exception:
            pass
        self._buf += data.replace("\r\n", "\n").replace("\r", "\n")
        while "\n" in self._buf:
            line, self._buf = self._buf.split("\n", 1)
            log_hub.emit(line)
        return len(data)

    def flush(self) -> None:
        try:
            self._stream.flush()
        except Exception:
            pass
        leftover = self._buf.strip()
        if leftover:
            log_hub.emit(leftover)
            self._buf = ""

    def isatty(self) -> bool:
        return False

    def fileno(self):
        return self._stream.fileno()

    def __getattr__(self, name):
        return getattr(self._stream, name)


log_hub = LogHub()
_installed = False


def install_quiet_ws_logs() -> None:
    import logging

    class _QuietWs(logging.Filter):
        def filter(self, record: logging.LogRecord) -> bool:
            blob = record.getMessage()
            if record.exc_info and record.exc_info[1] is not None:
                blob += " " + str(record.exc_info[1])
            return not _is_ws_noise(blob)

    quiet = _QuietWs()
    for name in (
        "websockets",
        "websockets.protocol",
        "websockets.legacy",
        "websockets.legacy.protocol",
        "uvicorn.error",
        "uvicorn",
        "asyncio",
    ):
        logging.getLogger(name).addFilter(quiet)


def install_capture() -> None:
    global _installed
    if _installed:
        return
    _installed = True
    install_quiet_ws_logs()
    sys.stdout = _Tee(sys.stdout)
    sys.stderr = _Tee(sys.stderr)
    print("[log] dashboard capture enabled")

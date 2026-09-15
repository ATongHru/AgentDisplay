"""Cursor hook event queue bridge (atomic drain)."""

from __future__ import annotations

import json
import os
import threading
import time
from pathlib import Path
from typing import Callable

HOOK_QUEUE = Path(
    os.getenv(
        "AGENT_DISPLAY_QUEUE",
        str(Path.home() / ".cursor" / "hooks" / "event_queue.jsonl"),
    )
)
HOOK_ONLINE = HOOK_QUEUE.parent / "backend_online.json"

_hook_stop = threading.Event()
_hook_last_online = 0.0


def touch_hook_online(force: bool = False) -> None:
    global _hook_last_online
    now = time.monotonic()
    if not force and now - _hook_last_online < 2.0:
        return
    try:
        HOOK_ONLINE.parent.mkdir(parents=True, exist_ok=True)
        HOOK_ONLINE.write_text(json.dumps({"ok": True, "ts": time.time()}), encoding="utf-8")
        _hook_last_online = now
    except OSError as exc:
        print(f"[hook] online file failed: {exc}")


def drain_hook_queue(apply_line: Callable[[dict], None]) -> int:
    """Atomically take the queue file and apply each JSON line. Returns applied count."""
    if not HOOK_QUEUE.exists() or HOOK_QUEUE.stat().st_size == 0:
        return 0
    processing = HOOK_QUEUE.with_suffix(".processing")
    try:
        HOOK_QUEUE.rename(processing)
    except OSError:
        return 0
    try:
        raw = processing.read_text(encoding="utf-8")
        processing.unlink(missing_ok=True)
    except OSError as exc:
        print(f"[hook] queue read failed: {exc}")
        try:
            processing.unlink(missing_ok=True)
        except OSError:
            pass
        return 0

    applied = 0
    for line in raw.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            data = json.loads(line)
            apply_line(data)
            applied += 1
            print(f"[hook] applied {line}")
        except (json.JSONDecodeError, ValueError, TypeError) as exc:
            print(f"[hook] skipped: {exc} -> {line}")
    return applied


def run_hook_loop(apply_line: Callable[[dict], None], stop_event: threading.Event | None = None) -> None:
    stop = stop_event or _hook_stop
    HOOK_QUEUE.parent.mkdir(parents=True, exist_ok=True)
    touch_hook_online(True)
    print(f"[hook] watching {HOOK_QUEUE}")
    while not stop.is_set():
        touch_hook_online()
        drain_hook_queue(apply_line)
        stop.wait(0.1)


def stop_hook_loop() -> None:
    _hook_stop.set()


def clear_hook_online() -> None:
    try:
        HOOK_ONLINE.unlink(missing_ok=True)
    except OSError:
        pass

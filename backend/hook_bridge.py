"""Cursor hook event queue bridge (atomic drain)."""

from __future__ import annotations

import json
import os
import threading
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Callable, Iterator

try:
    import msvcrt
except ImportError:  # pragma: no cover - Windows is the supported desktop host
    msvcrt = None

HOOK_QUEUE = Path(
    os.getenv(
        "AGENT_DISPLAY_QUEUE",
        str(Path.home() / ".cursor" / "hooks" / "event_queue.jsonl"),
    )
)
HOOK_ONLINE = HOOK_QUEUE.parent / "backend_online.json"

_hook_stop = threading.Event()
_hook_last_online = 0.0


@contextmanager
def hook_queue_lock(queue: Path, timeout: float = 0.5) -> Iterator[bool]:
    """Coordinate queue append and rename on Windows with a sidecar byte lock."""
    lock_path = queue.with_suffix(queue.suffix + ".lock")
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+b") as handle:
        handle.seek(0)
        handle.write(b"\0")
        handle.flush()
        if msvcrt is None:
            yield True
            return
        deadline = time.monotonic() + timeout
        acquired = False
        while time.monotonic() < deadline:
            try:
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
                acquired = True
                break
            except OSError:
                time.sleep(0.01)
        try:
            yield acquired
        finally:
            if acquired:
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)


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
    processing = HOOK_QUEUE.with_suffix(".processing")
    with hook_queue_lock(HOOK_QUEUE) as locked:
        if not locked:
            return 0
        # A previous reader may have been interrupted after rename. Drain that
        # durable file before taking newer events from QUEUE_FILE.
        if not processing.exists():
            if not HOOK_QUEUE.exists() or HOOK_QUEUE.stat().st_size == 0:
                return 0
            try:
                HOOK_QUEUE.rename(processing)
            except OSError:
                return 0
    try:
        raw = processing.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"[hook] queue read failed: {exc}")
        # Never delete an unread queue.  Put it back when no new producer file
        # exists; otherwise retain .processing for manual recovery instead of
        # silently losing events.
        try:
            if not HOOK_QUEUE.exists():
                processing.rename(HOOK_QUEUE)
        except OSError as restore_exc:
            print(f"[hook] queue restore failed: {restore_exc}")
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
    try:
        processing.unlink(missing_ok=True)
    except OSError as exc:
        print(f"[hook] processed queue cleanup failed: {exc}")
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

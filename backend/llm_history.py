"""Persistent LLM chat history under project log/ directory."""

from __future__ import annotations

import json
import os
import threading
import uuid
from collections import deque
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = ROOT / "log"
HISTORY_FILE = LOG_DIR / "llm_chat.jsonl"
MAX_HISTORY_LINES = int(os.getenv("LLM_HISTORY_MAX_LINES", "2000"))
QUERY_TAIL_LINES = int(os.getenv("LLM_HISTORY_QUERY_TAIL", "5000"))

_lock = threading.Lock()


def _ensure_dir() -> None:
    LOG_DIR.mkdir(parents=True, exist_ok=True)


def _now_iso() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")


def _rotate_if_needed() -> None:
    if not HISTORY_FILE.is_file():
        return
    try:
        lines = HISTORY_FILE.read_text(encoding="utf-8").splitlines()
    except OSError:
        return
    if len(lines) <= MAX_HISTORY_LINES:
        return
    keep = lines[-MAX_HISTORY_LINES:]
    HISTORY_FILE.write_text("\n".join(keep) + ("\n" if keep else ""), encoding="utf-8")
    print(f"[llm-history] rotated to last {len(keep)} lines")


def append_record(
    *,
    user: str,
    assistant: str,
    model: str = "",
    usage: dict[str, Any] | None = None,
    source: str = "VOICE",
    session_id: str = "",
    latency_ms: int | None = None,
    error: str = "",
) -> dict[str, Any]:
    """Append one LLM turn to log/llm_chat.jsonl. Returns the stored record."""
    usage = usage or {}
    record = {
        "id": uuid.uuid4().hex[:16],
        "ts": _now_iso(),
        "source": (source or "VOICE").upper(),
        "session_id": session_id or "",
        "model": model or "",
        "user": (user or "").strip(),
        "assistant": (assistant or "").strip(),
        "prompt_tokens": int(usage.get("prompt_tokens") or usage.get("input_tokens") or 0),
        "completion_tokens": int(usage.get("completion_tokens") or usage.get("output_tokens") or 0),
        "total_tokens": int(usage.get("total_tokens") or 0),
        "latency_ms": int(latency_ms) if latency_ms is not None else None,
        "error": (error or "").strip(),
    }
    if record["total_tokens"] <= 0:
        record["total_tokens"] = record["prompt_tokens"] + record["completion_tokens"]

    line = json.dumps(record, ensure_ascii=False)
    with _lock:
        _ensure_dir()
        with HISTORY_FILE.open("a", encoding="utf-8") as fh:
            fh.write(line + "\n")
        _rotate_if_needed()
    return record


def _tail_records(max_lines: int | None = None) -> list[dict[str, Any]]:
    if not HISTORY_FILE.is_file():
        return []
    limit = max_lines or QUERY_TAIL_LINES
    try:
        lines = deque(HISTORY_FILE.open("r", encoding="utf-8"), maxlen=limit)
    except OSError:
        return []
    items: list[dict[str, Any]] = []
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            items.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return items


def query_records(
    *,
    page: int = 1,
    page_size: int = 20,
    q: str = "",
    source: str = "",
    date_from: str = "",
    date_to: str = "",
) -> dict[str, Any]:
    page = max(1, int(page or 1))
    page_size = min(100, max(1, int(page_size or 20)))
    q = (q or "").strip().lower()
    source = (source or "").strip().upper()
    date_from = (date_from or "").strip()
    date_to = (date_to or "").strip()

    with _lock:
        items = _tail_records()

    items.reverse()

    def match(rec: dict[str, Any]) -> bool:
        ts = str(rec.get("ts") or "")
        if date_from and ts[:10] < date_from:
            return False
        if date_to and ts[:10] > date_to:
            return False
        if source and str(rec.get("source") or "").upper() != source:
            return False
        if q:
            blob = " ".join(
                [
                    str(rec.get("user") or ""),
                    str(rec.get("assistant") or ""),
                    str(rec.get("model") or ""),
                    str(rec.get("session_id") or ""),
                    str(rec.get("error") or ""),
                ]
            ).lower()
            if q not in blob:
                return False
        return True

    filtered = [r for r in items if match(r)]
    total = len(filtered)
    pages = max(1, (total + page_size - 1) // page_size)
    if page > pages:
        page = pages
    start = (page - 1) * page_size
    slice_ = filtered[start : start + page_size]

    token_sum = sum(int(r.get("total_tokens") or 0) for r in filtered)
    return {
        "ok": True,
        "page": page,
        "page_size": page_size,
        "total": total,
        "pages": pages,
        "total_tokens": token_sum,
        "items": slice_,
    }

"""Per-device conversation context shared across voice turns."""

from __future__ import annotations

import os
import threading
import time
from typing import Any


# How long of silence starts a fresh conversation context.
CONTEXT_IDLE_SEC = float(os.getenv("CHAT_CONTEXT_IDLE_SEC", "300"))
# Keep this many user/assistant pairs (token budget guard).
CONTEXT_MAX_TURNS = int(os.getenv("CHAT_CONTEXT_MAX_TURNS", "8"))


class ChatContextStore:
    def __init__(self, idle_sec: float = CONTEXT_IDLE_SEC, max_turns: int = CONTEXT_MAX_TURNS):
        self._idle_sec = idle_sec
        self._max_turns = max(1, max_turns)
        self._lock = threading.Lock()
        self._messages: list[dict[str, str]] = []
        self._last_active = 0.0

    def clear(self) -> None:
        with self._lock:
            self._messages.clear()
            self._last_active = 0.0
            print("[chat] context cleared")

    def history_for_llm(self) -> list[dict[str, str]]:
        """Return prior turns for the current conversation (not including the new user line)."""
        with self._lock:
            self._expire_locked()
            return [dict(m) for m in self._messages]

    def add_turn(self, user_text: str, assistant_text: str) -> None:
        user_text = (user_text or "").strip()
        assistant_text = (assistant_text or "").strip()
        if not user_text or not assistant_text:
            return
        with self._lock:
            self._expire_locked()
            self._messages.append({"role": "user", "content": user_text})
            self._messages.append({"role": "assistant", "content": assistant_text})
            # Trim to last N turns (2 messages each).
            max_msgs = self._max_turns * 2
            if len(self._messages) > max_msgs:
                self._messages = self._messages[-max_msgs:]
            self._last_active = time.monotonic()
            turns = len(self._messages) // 2
            print(f"[chat] context turns={turns} idle_sec={self._idle_sec}")

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            self._expire_locked()
            return {
                "turns": len(self._messages) // 2,
                "idle_sec": self._idle_sec,
                "max_turns": self._max_turns,
                "messages": [dict(m) for m in self._messages],
            }

    def _expire_locked(self) -> None:
        if not self._messages or self._last_active <= 0:
            return
        if (time.monotonic() - self._last_active) >= self._idle_sec:
            self._messages.clear()
            self._last_active = 0.0
            print(f"[chat] context expired after {self._idle_sec:.0f}s idle")


# Single-device product: one shared conversation context.
chat_context = ChatContextStore()

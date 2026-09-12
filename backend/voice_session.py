"""In-memory voice session state for ESP32 upload / TTS pull."""

from __future__ import annotations

import threading
import time
import uuid
from collections import deque
from dataclasses import dataclass, field
from enum import Enum
from typing import Deque


class SessionPhase(str, Enum):
    RECEIVED = "received"
    ASR = "asr"
    LLM = "llm"
    TTS = "tts"
    READY = "ready"
    PLAYING = "playing"
    DONE = "done"
    ERROR = "error"


CHUNK_SIZE = 4096
SESSION_TIMEOUT_SEC = 60.0


@dataclass
class VoiceSession:
    session_id: str
    pcm_data: bytes
    sample_rate: int = 16000
    channels: int = 1
    bit_depth: int = 16
    created_at: float = field(default_factory=time.time)
    phase: SessionPhase = SessionPhase.RECEIVED
    asr_text: str = ""
    llm_text: str = ""
    error: str = ""
    audio_pcm: bytes = b""
    audio_chunks: Deque[bytes] = field(default_factory=deque)
    audio_end: bool = False
    audio_pull_offset: int = 0
    lock: threading.Lock = field(default_factory=threading.Lock)

    def age(self) -> float:
        return time.time() - self.created_at

    def expired(self) -> bool:
        return self.age() > SESSION_TIMEOUT_SEC

    def set_error(self, message: str) -> None:
        with self.lock:
            self.phase = SessionPhase.ERROR
            self.error = message
            self.audio_end = True

    def set_asr(self, text: str) -> None:
        with self.lock:
            self.asr_text = text
            self.phase = SessionPhase.LLM

    def append_llm(self, text: str) -> None:
        with self.lock:
            self.llm_text = text

    def set_tts_pcm(self, pcm: bytes) -> None:
        with self.lock:
            self.audio_pcm = pcm
            self.audio_chunks.clear()
            for i in range(0, len(pcm), CHUNK_SIZE):
                self.audio_chunks.append(pcm[i : i + CHUNK_SIZE])
            self.phase = SessionPhase.READY
            self.audio_end = len(self.audio_chunks) == 0

    def mark_playing(self) -> None:
        with self.lock:
            if self.phase == SessionPhase.READY:
                self.phase = SessionPhase.PLAYING

    def mark_done(self) -> None:
        with self.lock:
            self.phase = SessionPhase.DONE
            self.audio_end = True

    def pop_audio_chunk(self) -> tuple[bytes | None, bool]:
        """Return next PCM chunk and whether this is the final chunk."""
        with self.lock:
            if self.audio_chunks:
                chunk = self.audio_chunks.popleft()
                is_end = not self.audio_chunks
                if is_end:
                    self.audio_end = True
                    self.phase = SessionPhase.DONE
                else:
                    # The session lock is already held here; reacquiring it would deadlock.
                    if self.phase == SessionPhase.READY:
                        self.phase = SessionPhase.PLAYING
                return chunk, is_end
            if self.audio_end:
                return None, True
            return None, False

    def to_dict(self) -> dict:
        with self.lock:
            return {
                "session_id": self.session_id,
                "phase": self.phase.value,
                "asr_text": self.asr_text,
                "llm_text": self.llm_text,
                "error": self.error,
                "audio_bytes": len(self.audio_pcm),
                "audio_chunks_pending": len(self.audio_chunks),
                "audio_end": self.audio_end,
                "age_sec": round(self.age(), 2),
            }


class VoiceSessionStore:
    def __init__(self) -> None:
        self._sessions: dict[str, VoiceSession] = {}
        self._lock = threading.Lock()

    def create(
        self,
        pcm_data: bytes,
        sample_rate: int = 16000,
        channels: int = 1,
        bit_depth: int = 16,
    ) -> VoiceSession:
        session_id = uuid.uuid4().hex[:12]
        session = VoiceSession(
            session_id=session_id,
            pcm_data=pcm_data,
            sample_rate=sample_rate,
            channels=channels,
            bit_depth=bit_depth,
        )
        with self._lock:
            self._cleanup_locked()
            self._sessions[session_id] = session
        return session

    def get(self, session_id: str) -> VoiceSession | None:
        with self._lock:
            session = self._sessions.get(session_id)
            if session and session.expired():
                del self._sessions[session_id]
                return None
            return session

    def delete(self, session_id: str) -> bool:
        with self._lock:
            return self._sessions.pop(session_id, None) is not None

    def list_sessions(self) -> list[dict]:
        with self._lock:
            self._cleanup_locked()
            return [s.to_dict() for s in self._sessions.values()]

    def _cleanup_locked(self) -> None:
        expired = [sid for sid, s in self._sessions.items() if s.expired()]
        for sid in expired:
            del self._sessions[sid]


session_store = VoiceSessionStore()

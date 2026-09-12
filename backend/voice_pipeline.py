"""Voice pipeline: ASR -> 2api.store LLM, optional TTS."""

from __future__ import annotations

import asyncio
import os
import subprocess
import tempfile
import threading
from typing import Callable

from asr import transcribe_pcm, warmup as asr_warmup
from llm import llm_chat, llm_configured
from voice_session import SessionPhase, VoiceSession, session_store

TTS_VOICE = os.getenv("TTS_VOICE", "zh-CN-XiaoxiaoNeural")
VOICE_TTS = os.getenv("VOICE_TTS", "0").strip() == "1"

TranscriptFn = Callable[..., None]
PushFn = Callable[[str, str, str], None]


def warmup() -> None:
    asr_warmup()


def transcribe_session_pcm(session: VoiceSession) -> str:
    session.phase = SessionPhase.ASR
    return transcribe_pcm(
        session.pcm_data,
        sample_rate=session.sample_rate,
        channels=session.channels,
        bit_depth=session.bit_depth,
    )


async def tts_to_pcm(text: str) -> bytes:
    import edge_tts

    if not text.strip():
        return b""

    communicate = edge_tts.Communicate(text, TTS_VOICE)
    with tempfile.NamedTemporaryFile(suffix=".mp3", delete=False) as tmp:
        mp3_path = tmp.name
    try:
        await communicate.save(mp3_path)
        pcm_path = mp3_path + ".pcm"
        cmd = [
            "ffmpeg", "-y", "-i", mp3_path,
            "-f", "s16le", "-acodec", "pcm_s16le", "-ar", "16000", "-ac", "1", pcm_path,
        ]
        proc = subprocess.run(cmd, capture_output=True, timeout=120)
        if proc.returncode != 0:
            raise RuntimeError(proc.stderr.decode("utf-8", errors="replace") or "ffmpeg failed")
        with open(pcm_path, "rb") as f:
            return f.read()
    finally:
        for path in (mp3_path, mp3_path + ".pcm"):
            try:
                os.unlink(path)
            except OSError:
                pass


def run_pipeline(session_id: str, push_event: PushFn, on_transcript: TranscriptFn | None) -> None:
    async def _run():
        session = session_store.get(session_id)
        if session is None:
            return
        try:
            push_event("THINKING", "正在识别", "VOICE")
            asr_text = await asyncio.to_thread(transcribe_session_pcm, session)
            if not asr_text:
                session.set_error("未识别到语音")
                push_event("ERROR", "未识别到语音", "VOICE")
                if on_transcript:
                    on_transcript(session_id, "")
                return
            session.set_asr(asr_text)
            if on_transcript:
                on_transcript(session_id, asr_text, "user")
            push_event("THINKING", asr_text, "VOICE")

            if not llm_configured():
                session.mark_done()
                push_event("IDLE", asr_text, "VOICE")
                print(f"[voice] asr {session_id}: {asr_text}")
                return

            def on_partial(partial: str) -> None:
                session.append_llm(partial)
                push_event("THINKING", partial, "VOICE")

            reply = await llm_chat(asr_text, on_partial)
            if not reply:
                session.set_error("LLM 无回复")
                push_event("ERROR", "LLM 无回复", "VOICE")
                return
            session.append_llm(reply)
            if on_transcript:
                on_transcript(session_id, reply, "assistant")
            if not VOICE_TTS:
                session.mark_done()
                push_event("IDLE", reply, "VOICE")
                print(f"[voice] llm {session_id}: {reply}")
                return
            session.phase = SessionPhase.TTS
            pcm = await tts_to_pcm(reply)
            session.set_tts_pcm(pcm)
            push_event("DONE", reply, "VOICE")
            print(f"[voice] session {session_id} ready audio={len(pcm)} bytes")
        except Exception as exc:
            print(f"[voice] session {session_id} failed: {exc}")
            session = session_store.get(session_id)
            if session:
                session.set_error(str(exc))
            push_event("ERROR", "处理失败", "VOICE")

    asyncio.run(_run())


def start_pipeline(session_id: str, push_event: PushFn, on_transcript: TranscriptFn | None = None) -> None:
    threading.Thread(
        target=run_pipeline,
        args=(session_id, push_event, on_transcript),
        daemon=True,
        name=f"voice-{session_id}",
    ).start()

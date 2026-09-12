"""Voice pipeline: ASR -> 2api.store LLM, optional TTS."""

from __future__ import annotations

import asyncio
import os
import threading
import time
from typing import Callable

from asr import transcribe_pcm, warmup as asr_warmup
from chat_context import chat_context
from llm import llm_chat, llm_configured
from llm_history import append_record
from settings_store import get_volume_percent
from voice_session import SessionPhase, VoiceSession, session_store

TTS_VOICE = os.getenv("TTS_VOICE", "zh-CN-XiaoxiaoNeural")
VOICE_TTS = os.getenv("VOICE_TTS", "1").strip() not in {"0", "false", "False", ""}
# Same edge-tts engine; faster speech shortens synthesis + transfer.
TTS_RATE = os.getenv("TTS_RATE", "+20%").strip() or "+20%"

TranscriptFn = Callable[..., None]
PushFn = Callable[[str, str, str], None]
AppendFn = Callable[..., None]


class _DeltaCoalescer:
    def __init__(self, flush_fn: Callable[[str], None], min_chars: int = 6, min_sec: float = 0.12):
        self._flush_fn = flush_fn
        self._min_chars = min_chars
        self._min_sec = min_sec
        self._buf = ""
        self._last = 0.0
        self._lock = threading.Lock()

    def add(self, delta: str) -> None:
        if not delta:
            return
        with self._lock:
            self._buf += delta
            now = time.monotonic()
            if len(self._buf) >= self._min_chars or now - self._last >= self._min_sec:
                self._emit_locked()

    def flush(self) -> None:
        with self._lock:
            self._emit_locked()

    def _emit_locked(self) -> None:
        if not self._buf:
            return
        text = self._buf
        self._buf = ""
        self._last = time.monotonic()
        self._flush_fn(text)


def warmup() -> None:
    print(f"[voice] TTS={'on' if VOICE_TTS else 'off'} voice={TTS_VOICE} rate={TTS_RATE}")
    asr_warmup()


def transcribe_session_pcm(session: VoiceSession) -> str:
    session.phase = SessionPhase.ASR
    return transcribe_pcm(
        session.pcm_data,
        sample_rate=session.sample_rate,
        channels=session.channels,
        bit_depth=session.bit_depth,
    )


def _split_tts_parts(text: str) -> list[str]:
    """Split reply into short clauses so first audio can start early."""
    text = (text or "").strip()
    if not text:
        return []
    hard = set("。！？!?;；\n")
    soft = set("，、,;； ")
    parts: list[str] = []
    buf = ""
    for ch in text:
        buf += ch
        strip = buf.strip()
        if not strip:
            buf = ""
            continue
        if ch in hard and len(strip) >= 2:
            parts.append(strip)
            buf = ""
        elif len(strip) >= 28 and ch in soft:
            parts.append(strip)
            buf = ""
        elif len(strip) >= 48:
            parts.append(strip)
            buf = ""
    if buf.strip():
        parts.append(buf.strip())
    merged: list[str] = []
    for part in parts:
        if merged and len(part) < 4:
            merged[-1] = merged[-1] + part
        else:
            merged.append(part)
    return merged or [text]


async def tts_to_pcm(text: str) -> bytes:
    import edge_tts

    if not text.strip():
        return b""

    communicate = edge_tts.Communicate(text, TTS_VOICE, rate=TTS_RATE)
    mp3 = bytearray()
    async for chunk in communicate.stream():
        if chunk["type"] == "audio":
            mp3.extend(chunk["data"])
    if not mp3:
        raise RuntimeError("TTS 无音频")
    pcm = await asyncio.to_thread(_mp3_to_pcm16k_mono, bytes(mp3))
    # Volume is applied on-device only (avoid double attenuation with firmware gain).
    volume = get_volume_percent()
    print(f"[tts] chars={len(text)} mp3={len(mp3)} pcm={len(pcm)} rate={TTS_RATE} device_volume={volume}%")
    return pcm


async def synthesize_to_session(session: VoiceSession, text: str) -> int:
    """Stream sentence-level TTS into the session so WS pump can start early."""
    parts = _split_tts_parts(text)
    total = 0
    if not parts:
        session.finish_tts()
        return 0
    session.phase = SessionPhase.TTS
    for idx, part in enumerate(parts):
        t0 = time.monotonic()
        pcm = await tts_to_pcm(part)
        dt = time.monotonic() - t0
        if not pcm:
            continue
        session.append_tts_pcm(pcm)
        total += len(pcm)
        print(
            f"[tts] part {idx + 1}/{len(parts)} chars={len(part)} "
            f"pcm={len(pcm)} in {dt:.2f}s queued={total}"
        )
    session.finish_tts()
    return total


def _mp3_to_pcm16k_mono(mp3: bytes) -> bytes:
    import array
    import miniaudio

    decoded = miniaudio.decode(
        mp3,
        output_format=miniaudio.SampleFormat.SIGNED16,
        nchannels=1,
        sample_rate=16000,
    )
    samples = decoded.samples
    if isinstance(samples, array.array) and samples.typecode == "f":
        pcm = array.array(
            "h",
            (max(-32767, min(32767, int(x * 32767))) for x in samples),
        )
        return pcm.tobytes()
    if hasattr(samples, "tobytes"):
        return samples.tobytes()
    return bytes(samples)


def run_pipeline(
    session_id: str,
    push_event: PushFn,
    on_transcript: TranscriptFn | None,
    on_done: Callable[[], None] | None = None,
    push_append: AppendFn | None = None,
) -> None:
    async def _run():
        session = session_store.get(session_id)
        if session is None:
            return
        try:
            print(
                f"[voice] session {session_id} pcm={len(session.pcm_data)} "
                f"sr={session.sample_rate} ch={session.channels}"
            )
            push_event("THINKING", "正在识别", "VOICE")
            if session.asr_ready and session.asr_text:
                asr_text = session.asr_text.strip()
                print(f"[voice] asr prefilled session={session_id} text={asr_text!r}")
            else:
                asr_text = await asyncio.to_thread(transcribe_session_pcm, session)
            if not asr_text:
                session.mark_done()
                push_event("IDLE", "没听清，请再说一次", "VOICE")
                if on_transcript:
                    on_transcript(session_id, "")
                print(f"[voice] asr empty {session_id}, ask retry")
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

            last = ""
            started = False
            tts_q: asyncio.Queue[str | None] = asyncio.Queue()
            tts_idx = 0

            def take_ready(full: str, final: bool) -> list[str]:
                nonlocal tts_idx
                chunk = full[tts_idx:]
                last_cut = 0
                out: list[str] = []
                for i, ch in enumerate(chunk):
                    if ch in "。！？!?；;\n":
                        piece = chunk[last_cut : i + 1].strip()
                        if len(piece) >= 2:
                            out.append(piece)
                            last_cut = i + 1
                if final:
                    piece = chunk[last_cut:].strip()
                    if piece:
                        out.append(piece)
                        last_cut = len(chunk)
                tts_idx += last_cut
                return out

            async def tts_worker() -> int:
                total = 0
                while True:
                    part = await tts_q.get()
                    if part is None:
                        break
                    t0 = time.monotonic()
                    pcm = await tts_to_pcm(part)
                    dt = time.monotonic() - t0
                    if not pcm:
                        continue
                    session.append_tts_pcm(pcm)
                    total += len(pcm)
                    print(
                        f"[tts] live chars={len(part)} pcm={len(pcm)} "
                        f"in {dt:.2f}s queued={total}"
                    )
                return total

            def emit_delta(delta: str) -> None:
                if push_append:
                    push_append(delta, "VOICE", False)

            coalescer = _DeltaCoalescer(emit_delta)
            worker = asyncio.create_task(tts_worker()) if VOICE_TTS else None

            def on_partial(full: str) -> None:
                nonlocal last, started
                session.append_llm(full)
                delta = full[len(last):] if full.startswith(last) else full
                last = full
                if not delta:
                    return
                if not started:
                    started = True
                    if push_append:
                        push_append("", "VOICE", True)
                coalescer.add(delta)
                if VOICE_TTS:
                    for part in take_ready(full, False):
                        tts_q.put_nowait(part)

            try:
                history = chat_context.history_for_llm()
                llm_result = await llm_chat(asr_text, on_partial, history=history)
                coalescer.flush()
                if isinstance(llm_result, dict):
                    reply = (llm_result.get("text") or "").strip()
                    usage = llm_result.get("usage") or {}
                    model = llm_result.get("model") or ""
                    latency_ms = llm_result.get("latency_ms")
                else:
                    reply = str(llm_result or "").strip()
                    usage, model, latency_ms = {}, "", None
                if not reply:
                    append_record(
                        user=asr_text,
                        assistant="",
                        model=model,
                        usage=usage,
                        source="VOICE",
                        session_id=session_id,
                        latency_ms=latency_ms,
                        error="LLM 无回复",
                    )
                    session.set_error("LLM 无回复")
                    push_event("ERROR", "LLM 无回复", "VOICE")
                    return
                chat_context.add_turn(asr_text, reply)
                append_record(
                    user=asr_text,
                    assistant=reply,
                    model=model,
                    usage=usage,
                    source="VOICE",
                    session_id=session_id,
                    latency_ms=latency_ms,
                )
                session.append_llm(reply)
                if on_transcript:
                    on_transcript(session_id, reply, "assistant")
                tok = int((usage or {}).get("total_tokens") or 0)
                print(f"[voice] llm {session_id}: tokens={tok} latency_ms={latency_ms} {reply[:80]}")
                if not VOICE_TTS:
                    session.mark_done()
                    push_event("IDLE", reply, "VOICE")
                    return
                for part in take_ready(reply, True):
                    await tts_q.put(part)
                await tts_q.put(None)
                total = await worker
                session.finish_tts()
                print(f"[voice] session {session_id} ready audio={total} bytes (streamed)")
            finally:
                if worker is not None and not worker.done():
                    tts_q.put_nowait(None)
                    try:
                        await worker
                    except Exception:
                        pass
        except Exception as exc:
            import traceback

            print(f"[voice] session {session_id} failed: {exc}")
            traceback.print_exc()
            session = session_store.get(session_id)
            if session:
                session.set_error(str(exc))
            detail = str(exc).strip().replace("\n", " ")
            if len(detail) > 80:
                detail = detail[:77] + "..."
            push_event("ERROR", f"处理失败：{detail}" if detail else "处理失败", "VOICE")
        finally:
            if on_done:
                on_done()

    asyncio.run(_run())


def start_pipeline(
    session_id: str,
    push_event: PushFn,
    on_transcript: TranscriptFn | None = None,
    on_done: Callable[[], None] | None = None,
    push_append: AppendFn | None = None,
) -> None:
    threading.Thread(
        target=run_pipeline,
        args=(session_id, push_event, on_transcript, on_done, push_append),
        daemon=True,
        name=f"voice-{session_id}",
    ).start()

"""Voice pipeline: ASR -> 2api.store LLM, optional TTS."""

from __future__ import annotations

import asyncio
import os
import time
from typing import Callable

from asr import transcribe_pcm, warmup as asr_warmup
from chat_context import chat_context
from llm import llm_chat, llm_configured
from llm_history import append_record
from settings_store import get_volume_percent, get_tts_voice
from tts_sapi import is_local_voice, synth_local_pcm
from voice_session import SessionPhase, VoiceSession, session_store

VOICE_TTS = os.getenv("VOICE_TTS", "0").strip() not in {"0", "false", "False", ""}
TTS_RATE = os.getenv("TTS_RATE", "+20%").strip() or "+20%"

TranscriptFn = Callable[..., None]
PushFn = Callable[..., None]
AppendFn = Callable[..., None]

_loop: asyncio.AbstractEventLoop | None = None
_running_tasks: dict[str, asyncio.Task] = {}


class _DeltaCoalescer:
    def __init__(self, flush_fn: Callable[[str], None], min_chars: int = 6, min_sec: float = 0.12):
        self._flush_fn = flush_fn
        self._min_chars = min_chars
        self._min_sec = min_sec
        self._buf = ""
        self._last = 0.0

    def add(self, delta: str) -> None:
        if not delta:
            return
        now = time.monotonic()
        self._buf += delta
        if len(self._buf) >= self._min_chars or now - self._last >= self._min_sec:
            self._emit()

    def flush(self) -> None:
        self._emit()

    def _emit(self) -> None:
        if not self._buf:
            return
        text = self._buf
        self._buf = ""
        self._last = time.monotonic()
        self._flush_fn(text)


def bind_loop(loop: asyncio.AbstractEventLoop) -> None:
    global _loop
    _loop = loop


def warmup() -> None:
    from tts_sapi import list_local_voices

    print(f"[voice] TTS={'on' if VOICE_TTS else 'off'} voice={get_tts_voice()} rate={TTS_RATE}")
    list_local_voices()
    asr_warmup()


def transcribe_session_pcm(session: VoiceSession) -> str:
    session.phase = SessionPhase.ASR
    return transcribe_pcm(
        bytes(session.pcm_data),
        sample_rate=session.sample_rate,
        channels=session.channels,
        bit_depth=session.bit_depth,
    )


def _split_tts_parts(text: str) -> list[str]:
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


def _tts_speakable(text: str) -> bool:
    for ch in text:
        if "\u4e00" <= ch <= "\u9fff" or ch.isalnum():
            return True
    return False


async def tts_to_pcm(text: str) -> bytes:
    text = (text or "").strip()
    if not text or not _tts_speakable(text):
        return b""

    preferred = get_tts_voice()
    if is_local_voice(preferred):
        try:
            pcm = await asyncio.to_thread(synth_local_pcm, text, preferred)
        except OSError as exc:
            print(f"[tts] {preferred} sapi failed ({exc})")
            return b""
        volume = get_volume_percent()
        print(
            f"[tts] chars={len(text)} pcm={len(pcm)} "
            f"voice={preferred} engine=sapi device_volume={volume}%"
        )
        return pcm

    import edge_tts

    async def _synth(voice: str, rate: str) -> bytes:
        communicate = edge_tts.Communicate(text, voice, rate=rate)
        mp3 = bytearray()
        async for chunk in communicate.stream():
            if chunk["type"] == "audio":
                mp3.extend(chunk["data"])
        if not mp3:
            raise RuntimeError("TTS 无音频")
        pcm = await asyncio.to_thread(_mp3_to_pcm16k_mono, bytes(mp3))
        volume = get_volume_percent()
        print(
            f"[tts] chars={len(text)} mp3={len(mp3)} pcm={len(pcm)} "
            f"voice={voice} rate={rate} device_volume={volume}%"
        )
        return pcm

    attempts = (
        (preferred, TTS_RATE),
        (preferred, "+0%"),
        ("zh-CN-XiaoxiaoNeural", TTS_RATE),
        ("zh-CN-XiaoxiaoNeural", "+0%"),
    )
    seen: set[tuple[str, str]] = set()
    last_exc: Exception | None = None
    for voice, rate in attempts:
        key = (voice, rate)
        if key in seen:
            continue
        seen.add(key)
        try:
            return await _synth(voice, rate)
        except (RuntimeError, OSError, TimeoutError) as exc:
            last_exc = exc
            print(f"[tts] {voice} rate={rate} failed ({exc})")
            await asyncio.sleep(0.25)
    print(f"[tts] skip unsynthable {text[:32]!r} err={last_exc}")
    return b""


async def synthesize_to_session(session: VoiceSession, text: str) -> int:
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


async def run_pipeline(
    session_id: str,
    push_event: PushFn,
    on_transcript: TranscriptFn | None,
    on_done: Callable[[], None] | None = None,
    push_append: AppendFn | None = None,
) -> None:
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
        push_event("THINKING", asr_text, "VOICE", "user")

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
                if _tts_speakable(piece) and len(piece) >= 2:
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
                try:
                    pcm = await tts_to_pcm(part)
                except (RuntimeError, OSError, TimeoutError) as exc:
                    print(f"[tts] skip part {part[:24]!r}: {exc}")
                    continue
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
            delta = full[len(last) :] if full.startswith(last) else full
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
            if push_append and reply and not started:
                push_append(reply, "VOICE", True, "assistant")
            tok = int((usage or {}).get("total_tokens") or 0)
            print(f"[voice] llm {session_id}: tokens={tok} latency_ms={latency_ms} {reply[:80]}")
            if not VOICE_TTS:
                session.mark_done()
                if not started:
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
                except (asyncio.CancelledError, RuntimeError):
                    pass
            session = session_store.get(session_id)
            if session is not None and VOICE_TTS:
                session.finish_tts()
    except asyncio.CancelledError:
        session = session_store.get(session_id)
        if session is not None:
            session.mark_done()
        raise
    except (RuntimeError, OSError, TimeoutError, ValueError) as exc:
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


def start_pipeline(
    session_id: str,
    push_event: PushFn,
    on_transcript: TranscriptFn | None = None,
    on_done: Callable[[], None] | None = None,
    push_append: AppendFn | None = None,
) -> asyncio.Task | None:
    loop = _loop
    if loop is None:
        raise RuntimeError("voice pipeline event loop not bound")

    async def _wrapped() -> None:
        try:
            await run_pipeline(
                session_id,
                push_event,
                on_transcript,
                on_done,
                push_append,
            )
        finally:
            _running_tasks.pop(session_id, None)

    task = loop.create_task(_wrapped(), name=f"voice-{session_id}")
    _running_tasks[session_id] = task
    return task


def cancel_pipeline(session_id: str) -> bool:
    task = _running_tasks.pop(session_id, None)
    if task is not None and not task.done():
        task.cancel()
    session = session_store.get(session_id)
    if session is not None:
        session.mark_done()
    session_store.delete(session_id)
    return task is not None


def cancel_all_pipelines() -> int:
    session_ids = list(_running_tasks.keys())
    for session_id in session_ids:
        cancel_pipeline(session_id)
    return len(session_ids)

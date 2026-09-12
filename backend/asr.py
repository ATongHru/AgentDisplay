"""Speech-to-text adapters (Vosk streaming / SpeechRecognition / faster-whisper)."""

from __future__ import annotations

import io
import json
import os
import threading
import wave
from pathlib import Path

ASR_ENGINE = os.getenv("ASR_ENGINE", "vosk").strip().lower()

# Vosk recommended feed size: 4096 bytes ≈ 128ms @ 16kHz s16le mono
VOSK_CHUNK_BYTES = 4096


def _default_vosk_path() -> str:
    root = Path(__file__).resolve().parent / "models"
    for name in ("vosk-model-small-cn-0.22", "vosk-model-cn-0.22"):
        here = root / name
        if here.exists():
            return str(here)
    for cand in Path("D:/0-C").glob("ESP32*N16R8/backend/models/vosk-model-small-cn-0.22"):
        if cand.exists():
            return str(cand)
    return str(root / "vosk-model-small-cn-0.22")


VOSK_MODEL_PATH = os.getenv("VOSK_MODEL_PATH", _default_vosk_path())
WHISPER_MODEL = os.getenv("WHISPER_MODEL", "base")
WHISPER_DEVICE = os.getenv("WHISPER_DEVICE", "cpu")

_vosk_model = None
_whisper_model = None
_lock = threading.Lock()


def _pcm_to_wav_path(pcm: bytes, sample_rate: int, channels: int, bit_depth: int) -> str:
    import tempfile

    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(bit_depth // 8)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm)
    tmp = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
    tmp.write(buf.getvalue())
    tmp.close()
    return tmp.name


def _get_vosk_model():
    global _vosk_model
    with _lock:
        if _vosk_model is None:
            model_dir = Path(VOSK_MODEL_PATH)
            if not model_dir.exists():
                raise RuntimeError(
                    "Vosk 中文模型未找到。请下载 vosk-model-small-cn-0.22 并解压到 "
                    f"{model_dir}，或设置环境变量 VOSK_MODEL_PATH。"
                    " 下载: https://alphacephei.com/vosk/models"
                )
            from vosk import Model, SetLogLevel

            SetLogLevel(-1)
            print(f"[asr] loading vosk model from {model_dir}")
            _vosk_model = Model(str(model_dir))
        return _vosk_model


def _get_faster_whisper():
    global _whisper_model
    with _lock:
        if _whisper_model is None:
            from faster_whisper import WhisperModel

            print(f"[asr] loading faster-whisper model={WHISPER_MODEL} device={WHISPER_DEVICE}")
            _whisper_model = WhisperModel(WHISPER_MODEL, device=WHISPER_DEVICE, compute_type="int8")
        return _whisper_model


def _pcm_to_mono_s16(pcm: bytes, channels: int, bit_depth: int) -> bytes:
    if bit_depth != 16:
        raise RuntimeError(f"Vosk 仅支持 16bit PCM，当前 bit_depth={bit_depth}")
    if channels == 1:
        return pcm
    if channels != 2:
        raise RuntimeError(f"不支持的声道数 channels={channels}")
    import array

    samples = array.array("h")
    samples.frombytes(pcm)
    mono = array.array("h", (samples[i] for i in range(0, len(samples), 2)))
    return mono.tobytes()


class VoskStreamRecognizer:
    """Feed PCM as it arrives; call finish() on audio_end."""

    def __init__(self, sample_rate: int = 16000, channels: int = 1, bit_depth: int = 16):
        from vosk import KaldiRecognizer, SetLogLevel

        SetLogLevel(-1)
        model = _get_vosk_model()
        self._channels = channels
        self._bit_depth = bit_depth
        self._rec = KaldiRecognizer(model, sample_rate)
        self._rec.SetWords(False)
        self._parts: list[str] = []
        self._partial = ""
        self._buf = bytearray()
        self._finished = False
        self._bytes = 0

    def accept(self, pcm: bytes) -> str:
        """Feed a PCM chunk. Returns latest partial text (may be empty)."""
        if self._finished or not pcm:
            return self._partial
        mono = _pcm_to_mono_s16(pcm, self._channels, self._bit_depth)
        self._buf.extend(mono)
        self._bytes += len(mono)
        while len(self._buf) >= VOSK_CHUNK_BYTES:
            chunk = bytes(self._buf[:VOSK_CHUNK_BYTES])
            del self._buf[:VOSK_CHUNK_BYTES]
            if self._rec.AcceptWaveform(chunk):
                data = json.loads(self._rec.Result())
                text = str(data.get("text", "")).strip()
                if text:
                    self._parts.append(text)
                self._partial = ""
            else:
                data = json.loads(self._rec.PartialResult())
                self._partial = str(data.get("partial", "")).strip()
        return self._partial

    def finish(self) -> str:
        if self._finished:
            return " ".join(self._parts).strip()
        self._finished = True
        if self._buf:
            self._rec.AcceptWaveform(bytes(self._buf))
            self._buf.clear()
        data = json.loads(self._rec.FinalResult())
        text = str(data.get("text", "")).strip()
        if text:
            self._parts.append(text)
        result = " ".join(self._parts).strip()
        print(f"[asr] vosk stream done bytes={self._bytes} text={result!r}")
        return result


def _transcribe_vosk_pcm(pcm: bytes, sample_rate: int, channels: int, bit_depth: int) -> str:
    stream = VoskStreamRecognizer(sample_rate, channels, bit_depth)
    # Feed in recommended chunk size even for one-shot uploads.
    for offset in range(0, len(pcm), VOSK_CHUNK_BYTES):
        stream.accept(pcm[offset : offset + VOSK_CHUNK_BYTES])
    return stream.finish()


def _transcribe_vosk(wav_path: str) -> str:
    with wave.open(wav_path, "rb") as wf:
        pcm = wf.readframes(wf.getnframes())
        return _transcribe_vosk_pcm(pcm, wf.getframerate(), wf.getnchannels(), wf.getsampwidth() * 8)


def _transcribe_google(wav_path: str) -> str:
    import speech_recognition as sr

    recognizer = sr.Recognizer()
    with sr.AudioFile(wav_path) as source:
        audio = recognizer.record(source)
    return recognizer.recognize_google(audio, language="zh-CN").strip()


def _transcribe_faster_whisper(wav_path: str) -> str:
    model = _get_faster_whisper()
    segments, _info = model.transcribe(wav_path, language="zh", beam_size=5)
    return "".join(seg.text for seg in segments).strip()


def transcribe_pcm(pcm: bytes, sample_rate: int = 16000, channels: int = 1, bit_depth: int = 16) -> str:
    if not pcm:
        return ""
    print(f"[asr] start engine={ASR_ENGINE} bytes={len(pcm)} sr={sample_rate} ch={channels} bits={bit_depth}")
    if ASR_ENGINE == "vosk":
        text = _transcribe_vosk_pcm(pcm, sample_rate, channels, bit_depth)
        print(f"[asr] vosk text={text!r}")
        return text
    wav_path = _pcm_to_wav_path(pcm, sample_rate, channels, bit_depth)
    try:
        if ASR_ENGINE == "google":
            text = _transcribe_google(wav_path)
        elif ASR_ENGINE in {"faster_whisper", "whisper"}:
            text = _transcribe_faster_whisper(wav_path)
        else:
            raise RuntimeError(f"unsupported ASR_ENGINE: {ASR_ENGINE}")
        print(f"[asr] {ASR_ENGINE} text={text!r}")
        return text
    finally:
        try:
            os.unlink(wav_path)
        except OSError:
            pass


def warmup() -> None:
    def _load():
        try:
            if ASR_ENGINE == "vosk":
                _get_vosk_model()
            elif ASR_ENGINE in {"faster_whisper", "whisper"}:
                _get_faster_whisper()
            print(f"[asr] warmup done engine={ASR_ENGINE}")
        except Exception as exc:
            print(f"[asr] warmup failed: {exc}")

    threading.Thread(target=_load, daemon=True, name="asr-warmup").start()

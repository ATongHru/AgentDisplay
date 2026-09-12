"""Speech-to-text adapters (Vosk / SpeechRecognition / faster-whisper)."""

from __future__ import annotations

import io
import json
import os
import threading
import wave
from pathlib import Path

ASR_ENGINE = os.getenv("ASR_ENGINE", "vosk").strip().lower()


def _default_vosk_path() -> str:
    here = Path(__file__).resolve().parent / "models" / "vosk-model-small-cn-0.22"
    if here.exists():
        return str(here)
    for cand in Path("D:/0-C").glob("ESP32*N16R8/backend/models/vosk-model-small-cn-0.22"):
        if cand.exists():
            return str(cand)
    return str(here)

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
            from vosk import Model

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


def _transcribe_vosk(wav_path: str) -> str:
    import speech_recognition as sr

    model = _get_vosk_model()
    recognizer = sr.Recognizer()
    with sr.AudioFile(wav_path) as source:
        audio = recognizer.record(source)
    raw = recognizer.recognize_vosk(audio, model)
    try:
        data = json.loads(raw)
        return str(data.get("text", "")).strip()
    except (json.JSONDecodeError, TypeError):
        return str(raw).strip()


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
    wav_path = _pcm_to_wav_path(pcm, sample_rate, channels, bit_depth)
    try:
        if ASR_ENGINE == "vosk":
            return _transcribe_vosk(wav_path)
        if ASR_ENGINE == "google":
            return _transcribe_google(wav_path)
        if ASR_ENGINE in {"faster_whisper", "whisper"}:
            return _transcribe_faster_whisper(wav_path)
        raise RuntimeError(f"unsupported ASR_ENGINE: {ASR_ENGINE}")
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

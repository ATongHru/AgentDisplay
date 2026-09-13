"""Offline TTS via Windows SAPI (no Microsoft cloud)."""

from __future__ import annotations

import os
import tempfile
import threading
import wave
from pathlib import Path

SSFM_CREATE_FOR_WRITE = 3
SAFT_16KHZ_16BIT_MONO = 18

_LOCK = threading.Lock()
_VOICES: list[dict] | None = None

_NAME_ZH = {
    "huihui": "慧慧",
    "yaoyao": "瑶瑶",
    "kangkang": "康康",
    "zira": "Zira",
    "david": "David",
    "mark": "Mark",
    "hazel": "Hazel",
}


def _token_from_id(raw: str) -> str:
    text = str(raw or "").strip()
    return text.rsplit("\\", 1)[-1] if text else ""


def _voice_id(token: str) -> str:
    return f"sapi:{token}"


def is_local_voice(voice_id: str) -> bool:
    return str(voice_id or "").startswith("sapi:")


def _short_name(name: str) -> str:
    lower = (name or "").lower()
    for en, zh in _NAME_ZH.items():
        if en in lower:
            return zh
    cleaned = (
        name.replace("Microsoft", "")
        .replace("Desktop", "")
        .replace("-", " ")
        .strip()
    )
    return cleaned.split()[0] if cleaned else (name or "系统语音")


def _gender_zh(gender: str) -> str:
    g = (gender or "").lower()
    if g == "female":
        return "女"
    if g == "male":
        return "男"
    return ""


def _sapi_rate() -> int:
    raw = (os.getenv("TTS_RATE") or "+20%").strip().replace("%", "")
    try:
        pct = int(raw)
    except ValueError:
        pct = 20
    return max(-10, min(10, int(round(pct / 10.0))))


def list_local_voices() -> list[dict[str, str]]:
    global _VOICES
    if _VOICES is not None:
        return list(_VOICES)
    with _LOCK:
        if _VOICES is not None:
            return list(_VOICES)
        voices: list[dict[str, str]] = []
        try:
            import pythoncom
            import win32com.client

            pythoncom.CoInitialize()
            speaker = None
            try:
                speaker = win32com.client.Dispatch("SAPI.SpVoice")
                for token in speaker.GetVoices():
                    tid = _token_from_id(token.Id)
                    if not tid:
                        continue
                    try:
                        name = str(token.GetAttribute("Name") or token.GetDescription() or tid)
                    except Exception:
                        name = tid
                    try:
                        gender = str(token.GetAttribute("Gender") or "")
                    except Exception:
                        gender = ""
                    try:
                        lang = str(token.GetAttribute("Language") or "").upper()
                    except Exception:
                        lang = ""
                    zh = lang in {"804", "0804", "2052"}
                    gender_zh = _gender_zh(gender)
                    short = _short_name(name)
                    loc = "本地中文" if zh else "本地系统"
                    detail = " · ".join(p for p in (gender_zh, loc) if p)
                    voices.append(
                        {
                            "id": _voice_id(tid),
                            "label": f"{short}（{detail}）" if detail else short,
                            "engine": "sapi",
                            "local": True,
                            "lang": "zh" if zh else "en",
                        }
                    )
            finally:
                speaker = None
                pythoncom.CoUninitialize()
        except Exception as exc:
            print(f"[tts] local SAPI unavailable: {exc}")
            _VOICES = []
            return []
        voices.sort(key=lambda item: (0 if item.get("lang") == "zh" else 1, item.get("label") or ""))
        _VOICES = voices
        if voices:
            print("[tts] local voices: " + ", ".join(v["label"] for v in voices))
        return list(_VOICES)


def synth_local_pcm(text: str, voice_id: str) -> bytes:
    text = (text or "").strip()
    token = str(voice_id or "")
    if token.startswith("sapi:"):
        token = token[5:]
    if not text or not token:
        return b""

    import pythoncom
    import win32com.client

    fd, path = tempfile.mkstemp(prefix="sapi_tts_", suffix=".wav")
    os.close(fd)
    wav_path = Path(path)
    try:
        with _LOCK:
            pythoncom.CoInitialize()
            speaker = None
            stream = None
            try:
                speaker = win32com.client.Dispatch("SAPI.SpVoice")
                matched = False
                for item in speaker.GetVoices():
                    if _token_from_id(item.Id) == token:
                        speaker.Voice = item
                        matched = True
                        break
                if not matched:
                    raise RuntimeError(f"SAPI 未找到音色 {token}")
                stream = win32com.client.Dispatch("SAPI.SpFileStream")
                stream.Format.Type = SAFT_16KHZ_16BIT_MONO
                stream.Open(str(wav_path), SSFM_CREATE_FOR_WRITE, False)
                speaker.AudioOutputStream = stream
                speaker.Rate = _sapi_rate()
                speaker.Speak(text, 0)
                stream.Close()
            finally:
                stream = None
                speaker = None
                pythoncom.CoUninitialize()
        with wave.open(str(wav_path), "rb") as wf:
            if wf.getsampwidth() != 2:
                raise RuntimeError("SAPI wav 不是 16bit")
            channels = wf.getnchannels()
            rate = wf.getframerate()
            frames = wf.readframes(wf.getnframes())
        if channels == 2:
            raw = bytearray()
            for i in range(0, len(frames), 4):
                raw.extend(frames[i : i + 2])
            frames = bytes(raw)
        if rate != 16000:
            raise RuntimeError(f"SAPI wav 采样率 {rate} 不是 16k")
        return frames
    finally:
        try:
            wav_path.unlink(missing_ok=True)
        except OSError:
            pass

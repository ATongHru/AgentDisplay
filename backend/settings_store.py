"""Persisted playback volume, mic switch, and TTS voice."""

from __future__ import annotations

import json
import os
import threading
from pathlib import Path

SETTINGS_PATH = Path(__file__).with_name("settings.json")
DEFAULT_VOLUME_PERCENT = 33
DEFAULT_VOICE_ENABLED = True
DEFAULT_TTS_VOICE = (os.getenv("TTS_VOICE") or "zh-CN-XiaoxiaoNeural").strip() or "zh-CN-XiaoxiaoNeural"

# Edge neural voices (Microsoft online). Local SAPI voices are appended at runtime.
TTS_VOICE_CHOICES: list[dict] = [
    {"id": "zh-CN-XiaoxiaoNeural", "label": "晓晓（女 · 微软在线）", "engine": "edge", "local": False},
    {"id": "zh-CN-XiaoyiNeural", "label": "晓伊（女 · 微软在线）", "engine": "edge", "local": False},
    {"id": "zh-CN-YunxiNeural", "label": "云希（男 · 微软在线）", "engine": "edge", "local": False},
    {"id": "zh-CN-YunyangNeural", "label": "云扬（男 · 微软在线）", "engine": "edge", "local": False},
    {"id": "zh-CN-YunjianNeural", "label": "云健（男 · 微软在线）", "engine": "edge", "local": False},
    {"id": "zh-CN-YunxiaNeural", "label": "云夏（男童 · 微软在线）", "engine": "edge", "local": False},
    {"id": "zh-CN-liaoning-XiaobeiNeural", "label": "晓北（辽宁 · 微软在线）", "engine": "edge", "local": False},
    {"id": "zh-CN-shaanxi-XiaoniNeural", "label": "晓妮（陕西 · 微软在线）", "engine": "edge", "local": False},
]
_TTS_VOICE_IDS = {item["id"] for item in TTS_VOICE_CHOICES}
_TTS_VOICE_ALIASES = {
    "zh-CN-YunhaoNeural": "zh-CN-YunyangNeural",
    "zh-CN-YunfengNeural": "zh-CN-YunjianNeural",
    "zh-CN-YunyeNeural": "zh-CN-YunxiNeural",
    "zh-CN-YunzeNeural": "zh-CN-YunyangNeural",
    "zh-CN-XiaoxuanNeural": "zh-CN-XiaoxiaoNeural",
    "zh-CN-XiaohanNeural": "zh-CN-XiaoxiaoNeural",
    "zh-CN-XiaomengNeural": "zh-CN-XiaoyiNeural",
    "zh-CN-XiaomoNeural": "zh-CN-XiaoxiaoNeural",
    "zh-CN-XiaoqiuNeural": "zh-CN-XiaoxiaoNeural",
    "zh-CN-XiaoruiNeural": "zh-CN-XiaoxiaoNeural",
    "zh-CN-XiaoyanNeural": "zh-CN-XiaoxiaoNeural",
    "zh-CN-XiaozhenNeural": "zh-CN-XiaoxiaoNeural",
    "zh-CN-XiaoshuangNeural": "zh-CN-XiaoyiNeural",
    "zh-CN-XiaoyouNeural": "zh-CN-XiaoyiNeural",
}

_lock = threading.Lock()
_volume_percent: int | None = None
_voice_enabled: bool | None = None
_tts_voice: str | None = None


def clamp_volume_percent(value: object) -> int:
    try:
        percent = int(round(float(value)))
    except (TypeError, ValueError):
        percent = DEFAULT_VOLUME_PERCENT
    return max(0, min(100, percent))


def _coerce_bool(value: object, default: bool = True) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return default
    if isinstance(value, (int, float)):
        return value != 0
    text = str(value).strip().lower()
    if text in {"1", "true", "yes", "on"}:
        return True
    if text in {"0", "false", "no", "off"}:
        return False
    return default


def normalize_tts_voice(value: object) -> str:
    text = str(value or "").strip()
    text = _TTS_VOICE_ALIASES.get(text, text)
    if text in _TTS_VOICE_IDS:
        return text
    if text.startswith("sapi:") and len(text) > 5:
        return text
    fallback = DEFAULT_TTS_VOICE
    fallback = _TTS_VOICE_ALIASES.get(fallback, fallback)
    if fallback in _TTS_VOICE_IDS:
        return fallback
    return "zh-CN-XiaoxiaoNeural"


def list_tts_voices(current: str | None = None) -> list[dict]:
    from tts_sapi import list_local_voices

    local = [dict(item) for item in list_local_voices()]
    online = [dict(item) for item in TTS_VOICE_CHOICES]
    voices = local + online
    ids = {str(item.get("id") or "") for item in voices}
    extra = str(current or "").strip()
    extra = _TTS_VOICE_ALIASES.get(extra, extra)
    if extra and extra not in ids:
        local_extra = extra.startswith("sapi:")
        voices.insert(
            len(local) if local_extra else len(voices),
            {
                "id": extra,
                "label": extra,
                "engine": "sapi" if local_extra else "edge",
                "local": local_extra,
            },
        )
    return voices


def _defaults() -> dict:
    return {
        "volume_percent": DEFAULT_VOLUME_PERCENT,
        "voice_enabled": DEFAULT_VOICE_ENABLED,
        "tts_voice": normalize_tts_voice(DEFAULT_TTS_VOICE),
    }


def _read_all() -> dict:
    if not SETTINGS_PATH.is_file():
        return _defaults()
    try:
        data = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise ValueError("bad settings")
    except (OSError, json.JSONDecodeError, ValueError):
        return _defaults()
    return {
        "volume_percent": clamp_volume_percent(data.get("volume_percent", DEFAULT_VOLUME_PERCENT)),
        "voice_enabled": _coerce_bool(data.get("voice_enabled", DEFAULT_VOICE_ENABLED)),
        "tts_voice": normalize_tts_voice(data.get("tts_voice", DEFAULT_TTS_VOICE)),
    }


def _write_all(volume_percent: int, voice_enabled: bool, tts_voice: str) -> None:
    payload = {
        "volume_percent": volume_percent,
        "voice_enabled": bool(voice_enabled),
        "tts_voice": tts_voice,
    }
    SETTINGS_PATH.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def _ensure_loaded() -> None:
    global _volume_percent, _voice_enabled, _tts_voice
    if _volume_percent is None or _voice_enabled is None or _tts_voice is None:
        data = _read_all()
        _volume_percent = data["volume_percent"]
        _voice_enabled = data["voice_enabled"]
        _tts_voice = data["tts_voice"]
        raw = None
        try:
            stored = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
            if isinstance(stored, dict):
                raw = str(stored.get("tts_voice") or "").strip()
        except (OSError, json.JSONDecodeError, ValueError):
            raw = None
        if raw != _tts_voice:
            _write_all(int(_volume_percent), bool(_voice_enabled), str(_tts_voice))
            print(f"[tts] migrated voice {raw!r} -> {_tts_voice}")


def get_volume_percent() -> int:
    with _lock:
        _ensure_loaded()
        return int(_volume_percent)


def set_volume_percent(value: object) -> int:
    global _volume_percent
    percent = clamp_volume_percent(value)
    with _lock:
        _ensure_loaded()
        _volume_percent = percent
        _write_all(percent, bool(_voice_enabled), str(_tts_voice))
    print(f"[volume] saved {percent}%")
    return percent


def get_voice_enabled() -> bool:
    with _lock:
        _ensure_loaded()
        return bool(_voice_enabled)


def set_voice_enabled(value: object) -> bool:
    global _voice_enabled
    enabled = _coerce_bool(value, DEFAULT_VOICE_ENABLED)
    with _lock:
        _ensure_loaded()
        _voice_enabled = enabled
        _write_all(int(_volume_percent), enabled, str(_tts_voice))
    print(f"[voice] enabled={enabled}")
    return enabled


def get_tts_voice() -> str:
    with _lock:
        _ensure_loaded()
        return str(_tts_voice)


def set_tts_voice(value: object) -> str:
    global _tts_voice
    voice = normalize_tts_voice(value)
    with _lock:
        _ensure_loaded()
        _tts_voice = voice
        _write_all(int(_volume_percent), bool(_voice_enabled), voice)
    print(f"[tts] voice={voice}")
    return voice


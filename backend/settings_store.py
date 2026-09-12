"""Persisted device playback volume and voice chat switch.

100% is the previous firmware 4x gain. Default 33% is one third of that loudness.
The backend scales TTS PCM so the change applies before the next firmware flash.
"""

from __future__ import annotations

import array
import json
import threading
from pathlib import Path

SETTINGS_PATH = Path(__file__).with_name("settings.json")
DEFAULT_VOLUME_PERCENT = 33
DEFAULT_VOICE_ENABLED = True

_lock = threading.Lock()
_volume_percent: int | None = None
_voice_enabled: bool | None = None


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


def _read_all() -> dict:
    if not SETTINGS_PATH.is_file():
        return {
            "volume_percent": DEFAULT_VOLUME_PERCENT,
            "voice_enabled": DEFAULT_VOICE_ENABLED,
        }
    try:
        data = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise ValueError("bad settings")
    except (OSError, json.JSONDecodeError, ValueError):
        return {
            "volume_percent": DEFAULT_VOLUME_PERCENT,
            "voice_enabled": DEFAULT_VOICE_ENABLED,
        }
    return {
        "volume_percent": clamp_volume_percent(data.get("volume_percent", DEFAULT_VOLUME_PERCENT)),
        "voice_enabled": _coerce_bool(data.get("voice_enabled", DEFAULT_VOICE_ENABLED)),
    }


def _write_all(volume_percent: int, voice_enabled: bool) -> None:
    payload = {
        "volume_percent": volume_percent,
        "voice_enabled": bool(voice_enabled),
    }
    SETTINGS_PATH.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def _ensure_loaded() -> None:
    global _volume_percent, _voice_enabled
    if _volume_percent is None or _voice_enabled is None:
        data = _read_all()
        _volume_percent = data["volume_percent"]
        _voice_enabled = data["voice_enabled"]


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
        _write_all(percent, bool(_voice_enabled))
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
        _write_all(int(_volume_percent), enabled)
    print(f"[voice] enabled={enabled}")
    return enabled


def scale_pcm16(pcm: bytes, percent: int | None = None) -> bytes:
    """Scale little-endian int16 PCM by volume percent (100 = unchanged)."""
    if not pcm:
        return pcm
    pct = get_volume_percent() if percent is None else clamp_volume_percent(percent)
    if pct == 100:
        return pcm
    raw = pcm if len(pcm) % 2 == 0 else pcm[:-1]
    samples = array.array("h")
    samples.frombytes(raw)
    for i, sample in enumerate(samples):
        value = (int(sample) * pct) // 100
        if value > 32767:
            value = 32767
        elif value < -32768:
            value = -32768
        samples[i] = value
    return samples.tobytes()

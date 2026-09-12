"""Persisted device playback volume.

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

_lock = threading.Lock()
_volume_percent: int | None = None


def clamp_volume_percent(value: object) -> int:
    try:
        percent = int(round(float(value)))
    except (TypeError, ValueError):
        percent = DEFAULT_VOLUME_PERCENT
    return max(0, min(100, percent))


def _read_file() -> int:
    if not SETTINGS_PATH.is_file():
        return DEFAULT_VOLUME_PERCENT
    try:
        data = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return DEFAULT_VOLUME_PERCENT
    return clamp_volume_percent(data.get("volume_percent", DEFAULT_VOLUME_PERCENT))


def _write_file(percent: int) -> None:
    payload = {"volume_percent": percent}
    SETTINGS_PATH.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def get_volume_percent() -> int:
    global _volume_percent
    with _lock:
        if _volume_percent is None:
            _volume_percent = _read_file()
        return _volume_percent


def set_volume_percent(value: object) -> int:
    global _volume_percent
    percent = clamp_volume_percent(value)
    with _lock:
        _volume_percent = percent
        _write_file(percent)
    print(f"[volume] saved {percent}%")
    return percent


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

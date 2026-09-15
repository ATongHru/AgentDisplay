"""Redact secrets from logs and API payloads."""

from __future__ import annotations

import re
from typing import Iterable

_WIFI_CMD_RE = re.compile(r"^(WIFI:)([^,]+),(.+)$", re.IGNORECASE)
_TOKEN_CMD_RE = re.compile(r"^(TOKEN:)(.+)$", re.IGNORECASE)
_BLE_PASS_RE = re.compile(r"(pass=)(\S+)", re.IGNORECASE)
_MASK = "********"


def mask_password(value: str | None) -> str:
    text = str(value or "")
    if not text:
        return ""
    return _MASK


def mask_wifi_profile(profile: dict, *, reveal_password: bool = False) -> dict:
    out = dict(profile)
    if not reveal_password and out.get("password"):
        out["password"] = _MASK
    return out


def mask_wifi_profiles(profiles: Iterable[dict], *, reveal_password: bool = False) -> list[dict]:
    return [mask_wifi_profile(p, reveal_password=reveal_password) for p in profiles]


def sanitize_ble_line(line: str, *, for_log: bool = True) -> str:
    text = (line or "").strip()
    if not text or not for_log:
        return text
    m = _WIFI_CMD_RE.match(text)
    if m:
        return f"{m.group(1)}{m.group(2)},{_MASK}"
    m = _TOKEN_CMD_RE.match(text)
    if m:
        return f"{m.group(1)}{_MASK}"
    return _BLE_PASS_RE.sub(rf"\1{_MASK}", text)

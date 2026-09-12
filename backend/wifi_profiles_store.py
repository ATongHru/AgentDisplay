"""Persisted ESP WiFi / backend connection profiles for the dashboard.

Stored in wifi_profiles.json (gitignored). These records can be edited in the UI
and pushed to the device via BLE provision.
"""

from __future__ import annotations

import json
import threading
import time
import uuid
from pathlib import Path
from typing import Any

PROFILES_PATH = Path(__file__).with_name("wifi_profiles.json")
MAX_PROFILES = 20

_lock = threading.Lock()
_profiles: list[dict[str, Any]] | None = None


def _empty() -> list[dict[str, Any]]:
    return []


def _normalize(item: dict[str, Any]) -> dict[str, Any] | None:
    ssid = str(item.get("ssid") or "").strip()
    host = str(item.get("host") or "").strip()
    if not ssid or not host:
        return None
    try:
        port = int(item.get("port") or 8000)
    except (TypeError, ValueError):
        port = 8000
    port = max(1, min(65535, port))
    name = str(item.get("name") or ssid).strip() or ssid
    pid = str(item.get("id") or uuid.uuid4().hex[:12])
    return {
        "id": pid,
        "name": name[:48],
        "ssid": ssid[:32],
        "password": str(item.get("password") or "")[:64],
        "host": host[:64],
        "port": port,
        "ip": str(item.get("ip") or "").strip()[:15],
        "netmask": str(item.get("netmask") or "").strip()[:15],
        "gateway": str(item.get("gateway") or "").strip()[:15],
        "updated_at": float(item.get("updated_at") or time.time()),
    }


def _read_file() -> list[dict[str, Any]]:
    if not PROFILES_PATH.is_file():
        return _empty()
    try:
        data = json.loads(PROFILES_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return _empty()
    raw = data.get("profiles") if isinstance(data, dict) else data
    if not isinstance(raw, list):
        return _empty()
    out: list[dict[str, Any]] = []
    for item in raw:
        if not isinstance(item, dict):
            continue
        norm = _normalize(item)
        if norm:
            out.append(norm)
    return out[:MAX_PROFILES]


def _write_file(profiles: list[dict[str, Any]]) -> None:
    payload = {"profiles": profiles}
    PROFILES_PATH.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def _ensure() -> list[dict[str, Any]]:
    global _profiles
    if _profiles is None:
        _profiles = _read_file()
    return _profiles


def list_profiles() -> list[dict[str, Any]]:
    with _lock:
        return [dict(p) for p in _ensure()]


def get_profile(profile_id: str) -> dict[str, Any] | None:
    with _lock:
        for p in _ensure():
            if p["id"] == profile_id:
                return dict(p)
    return None


def upsert_profile(payload: dict[str, Any]) -> dict[str, Any]:
    global _profiles
    norm = _normalize({**payload, "updated_at": time.time()})
    if norm is None:
        raise ValueError("ssid and host are required")
    with _lock:
        items = _ensure()
        for i, p in enumerate(items):
            if p["id"] == norm["id"] or (
                not payload.get("id")
                and p["ssid"] == norm["ssid"]
                and p["host"] == norm["host"]
                and int(p["port"]) == int(norm["port"])
            ):
                # keep existing id when matching by content
                if not payload.get("id"):
                    norm["id"] = p["id"]
                items[i] = norm
                _write_file(items)
                _profiles = items
                print(f"[wifi-profile] updated id={norm['id']} ssid={norm['ssid']}")
                return dict(norm)
        if len(items) >= MAX_PROFILES:
            raise ValueError(f"at most {MAX_PROFILES} profiles")
        items.append(norm)
        _write_file(items)
        _profiles = items
        print(f"[wifi-profile] created id={norm['id']} ssid={norm['ssid']}")
        return dict(norm)


def delete_profile(profile_id: str) -> bool:
    global _profiles
    with _lock:
        items = _ensure()
        next_items = [p for p in items if p["id"] != profile_id]
        if len(next_items) == len(items):
            return False
        _write_file(next_items)
        _profiles = next_items
        print(f"[wifi-profile] deleted id={profile_id}")
        return True

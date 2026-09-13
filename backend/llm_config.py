"""LLM runtime config in llm.json (gitignored), hot-editable from the dashboard.

Architecture: OpenAI-compatible Chat Completions over HTTP (streaming via httpx).
Supports multiple named profiles; one is active.
"""

from __future__ import annotations

import json
import os
import threading
import uuid
from pathlib import Path

LLM_PATH = Path(__file__).with_name("llm.json")
DEFAULT_BASE_URL = "https://2api.store"
DEFAULT_MODEL = "gpt-5.6-luna"
MAX_PROFILES = 8

_lock = threading.Lock()
_store: dict | None = None


def _from_env() -> dict[str, str]:
    return {
        "base_url": (os.getenv("LLM_BASE_URL", DEFAULT_BASE_URL).strip().rstrip("/") or DEFAULT_BASE_URL),
        "api_key": os.getenv("LLM_API_KEY", "").strip(),
        "model": (os.getenv("LLM_MODEL", DEFAULT_MODEL).strip() or DEFAULT_MODEL),
    }


def _new_id() -> str:
    return uuid.uuid4().hex[:12]


def _normalize_fields(data: dict) -> dict[str, str]:
    base = str(data.get("base_url") or data.get("url") or DEFAULT_BASE_URL).strip().rstrip("/")
    key = str(data.get("api_key") or data.get("key") or "").strip()
    model = str(data.get("model") or data.get("model_name") or DEFAULT_MODEL).strip()
    return {
        "base_url": base or DEFAULT_BASE_URL,
        "api_key": key,
        "model": model or DEFAULT_MODEL,
    }


def _profile_from_dict(data: dict, *, pid: str | None = None) -> dict[str, str]:
    fields = _normalize_fields(data)
    name = str(data.get("name") or "").strip()
    if not name:
        name = fields["model"] or "未命名"
    return {
        "id": (pid or str(data.get("id") or "").strip() or _new_id())[:32],
        "name": name[:32],
        **fields,
    }


def _empty_store() -> dict:
    profile = _profile_from_dict(_from_env(), pid="default")
    profile["name"] = "默认"
    return {"active": profile["id"], "profiles": [profile]}


def _parse_store(raw: object) -> dict:
    if isinstance(raw, dict) and isinstance(raw.get("profiles"), list):
        profiles: list[dict[str, str]] = []
        seen: set[str] = set()
        for item in raw["profiles"]:
            if not isinstance(item, dict):
                continue
            profile = _profile_from_dict(item)
            if profile["id"] in seen:
                profile["id"] = _new_id()
            seen.add(profile["id"])
            profiles.append(profile)
            if len(profiles) >= MAX_PROFILES:
                break
        if not profiles:
            return _empty_store()
        active = str(raw.get("active") or "")
        if not any(item["id"] == active for item in profiles):
            active = profiles[0]["id"]
        return {"active": active, "profiles": profiles}
    if isinstance(raw, dict):
        profile = _profile_from_dict(raw, pid="default")
        if not str(raw.get("name") or "").strip():
            profile["name"] = "默认"
        return {"active": profile["id"], "profiles": [profile]}
    return _empty_store()


def _mask_key(key: str) -> str:
    if not key:
        return ""
    if len(key) <= 8:
        return "*" * len(key)
    return key[:4] + ("*" * (len(key) - 8)) + key[-4:]


def _is_kept_key(raw_key: object) -> bool:
    if raw_key is None:
        return True
    if not isinstance(raw_key, str):
        return True
    text = raw_key.strip()
    return (not text) or set(text) <= {"*"}


def _public_profile(profile: dict[str, str], *, mask_key: bool) -> dict[str, str]:
    out = {
        "id": profile["id"],
        "name": profile["name"],
        "base_url": profile["base_url"],
        "api_key": _mask_key(profile["api_key"]) if mask_key else profile["api_key"],
        "model": profile["model"],
    }
    return out


def _public_store(store: dict, *, mask_key: bool) -> dict:
    active = _active_profile(store)
    return {
        "active": store["active"],
        "id": active["id"],
        "name": active["name"],
        "base_url": active["base_url"],
        "api_key": _mask_key(active["api_key"]) if mask_key else active["api_key"],
        "model": active["model"],
        "profiles": [_public_profile(item, mask_key=mask_key) for item in store["profiles"]],
    }


def _active_profile(store: dict) -> dict[str, str]:
    for item in store["profiles"]:
        if item["id"] == store["active"]:
            return item
    return store["profiles"][0]


def _find(store: dict, pid: str) -> dict[str, str] | None:
    for item in store["profiles"]:
        if item["id"] == pid:
            return item
    return None


def _write_file(store: dict) -> None:
    LLM_PATH.write_text(json.dumps(store, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _read_raw() -> object | None:
    if not LLM_PATH.is_file():
        return None
    try:
        data = json.loads(LLM_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return data


def _load_store(*, force: bool = False) -> dict:
    global _store
    with _lock:
        if _store is not None and not force:
            return {
                "active": _store["active"],
                "profiles": [dict(item) for item in _store["profiles"]],
            }
        raw = _read_raw()
        migrated = isinstance(raw, dict) and "profiles" not in raw
        store = _parse_store(raw) if raw is not None else _empty_store()
        try:
            if raw is None or migrated:
                _write_file(store)
                print(f"[llm] wrote {LLM_PATH.name} profiles={len(store['profiles'])}")
        except OSError as exc:
            print(f"[llm] write failed: {exc}")
        _store = {
            "active": store["active"],
            "profiles": [dict(item) for item in store["profiles"]],
        }
        active = _active_profile(_store)
        print(
            f"[llm] loaded {len(_store['profiles'])} profile(s) active={active['name']} "
            f"base_url={active['base_url']} model={active['model']} "
            f"key={'set' if active['api_key'] else 'empty'}"
        )
        return {
            "active": _store["active"],
            "profiles": [dict(item) for item in _store["profiles"]],
        }


def _save_store(store: dict) -> dict:
    global _store
    with _lock:
        _write_file(store)
        _store = {
            "active": store["active"],
            "profiles": [dict(item) for item in store["profiles"]],
        }
        return {
            "active": _store["active"],
            "profiles": [dict(item) for item in _store["profiles"]],
        }


def load_llm_config(*, force: bool = False) -> dict[str, str]:
    """Return the active profile (compat for llm.py)."""
    return dict(_active_profile(_load_store(force=force)))


def get_llm_config(*, mask_key: bool = False) -> dict:
    return _public_store(_load_store(), mask_key=mask_key)


def set_llm_config(payload: dict) -> dict:
    """Compat: update/create a profile. Activates it unless activate=False."""
    activate = bool(payload.get("activate", True))
    return upsert_llm_profile(payload, activate=activate)


def upsert_llm_profile(payload: dict, *, activate: bool = False) -> dict:
    store = _load_store()
    pid = str(payload.get("id") or "").strip()
    existing = _find(store, pid) if pid else None
    raw_key = payload.get("api_key")
    if existing is not None and _is_kept_key(raw_key):
        raw_key = existing["api_key"]
    elif _is_kept_key(raw_key) and existing is None:
        raw_key = ""
    merged = dict(payload)
    merged["api_key"] = raw_key
    profile = _profile_from_dict(merged, pid=existing["id"] if existing else None)
    if existing is None:
        if len(store["profiles"]) >= MAX_PROFILES:
            raise ValueError(f"最多保存 {MAX_PROFILES} 条 LLM 配置")
        store["profiles"].append(profile)
        if len(store["profiles"]) == 1:
            activate = True
    else:
        store["profiles"] = [profile if item["id"] == existing["id"] else item for item in store["profiles"]]
    if activate:
        store["active"] = profile["id"]
    saved = _save_store(store)
    print(
        f"[llm] saved id={profile['id']} name={profile['name']} "
        f"model={profile['model']} active={saved['active']}"
    )
    out = _public_store(saved, mask_key=True)
    out["saved_id"] = profile["id"]
    return out


def activate_llm_profile(pid: str) -> dict:
    store = _load_store()
    profile = _find(store, str(pid or "").strip())
    if profile is None:
        raise ValueError("配置不存在")
    store["active"] = profile["id"]
    saved = _save_store(store)
    print(f"[llm] active -> {profile['name']} ({profile['id']})")
    return _public_store(saved, mask_key=True)


def delete_llm_profile(pid: str) -> dict:
    store = _load_store()
    pid = str(pid or "").strip()
    profile = _find(store, pid)
    if profile is None:
        raise ValueError("配置不存在")
    if len(store["profiles"]) <= 1:
        raise ValueError("至少保留 1 条 LLM 配置")
    store["profiles"] = [item for item in store["profiles"] if item["id"] != pid]
    if store["active"] == pid:
        store["active"] = store["profiles"][0]["id"]
    saved = _save_store(store)
    print(f"[llm] deleted {pid}, active={saved['active']}")
    return _public_store(saved, mask_key=True)


def llm_settings_tuple() -> tuple[str, str, str]:
    cfg = load_llm_config()
    return cfg["base_url"], cfg["api_key"], cfg["model"]

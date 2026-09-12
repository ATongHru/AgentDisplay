"""LLM runtime config in llm.json (gitignored), hot-editable from the dashboard.

Architecture: OpenAI-compatible Chat Completions over HTTP (streaming via httpx).
Not LangChain — only base_url + api_key + model.
"""

from __future__ import annotations

import json
import os
import threading
from pathlib import Path

LLM_PATH = Path(__file__).with_name("llm.json")
DEFAULT_BASE_URL = "https://2api.store"
DEFAULT_MODEL = "gpt-5.6-luna"

_lock = threading.Lock()
_cache: dict[str, str] | None = None


def _from_env() -> dict[str, str]:
    return {
        "base_url": (os.getenv("LLM_BASE_URL", DEFAULT_BASE_URL).strip().rstrip("/") or DEFAULT_BASE_URL),
        "api_key": os.getenv("LLM_API_KEY", "").strip(),
        "model": (os.getenv("LLM_MODEL", DEFAULT_MODEL).strip() or DEFAULT_MODEL),
    }


def _normalize(data: dict) -> dict[str, str]:
    base = str(data.get("base_url") or data.get("url") or DEFAULT_BASE_URL).strip().rstrip("/")
    key = str(data.get("api_key") or data.get("key") or "").strip()
    model = str(data.get("model") or data.get("model_name") or DEFAULT_MODEL).strip()
    return {
        "base_url": base or DEFAULT_BASE_URL,
        "api_key": key,
        "model": model or DEFAULT_MODEL,
    }


def _read_file() -> dict[str, str] | None:
    if not LLM_PATH.is_file():
        return None
    try:
        data = json.loads(LLM_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(data, dict):
        return None
    return _normalize(data)


def _write_file(cfg: dict[str, str]) -> None:
    LLM_PATH.write_text(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def load_llm_config(*, force: bool = False) -> dict[str, str]:
    global _cache
    with _lock:
        if _cache is not None and not force:
            return dict(_cache)
        file_cfg = _read_file()
        if file_cfg is None:
            file_cfg = _from_env()
            try:
                _write_file(file_cfg)
                print(f"[llm] seeded {LLM_PATH.name} from env/defaults")
            except OSError as exc:
                print(f"[llm] seed write failed: {exc}")
        _cache = file_cfg
        print(
            f"[llm] loaded base_url={_cache['base_url']} model={_cache['model']} "
            f"key={'set' if _cache['api_key'] else 'empty'}"
        )
        return dict(_cache)


def get_llm_config(*, mask_key: bool = False) -> dict[str, str]:
    cfg = load_llm_config()
    if mask_key and cfg.get("api_key"):
        key = cfg["api_key"]
        if len(key) <= 8:
            cfg["api_key"] = "*" * len(key)
        else:
            cfg["api_key"] = key[:4] + ("*" * (len(key) - 8)) + key[-4:]
    return cfg


def set_llm_config(payload: dict) -> dict[str, str]:
    global _cache
    current = load_llm_config()
    raw_key = payload.get("api_key", current["api_key"])
    if isinstance(raw_key, str) and raw_key and set(raw_key) <= {"*"}:
        raw_key = current["api_key"]
    elif raw_key is None:
        raw_key = current["api_key"]
    cfg = _normalize(
        {
            "base_url": payload.get("base_url", current["base_url"]),
            "api_key": raw_key,
            "model": payload.get("model", current["model"]),
        }
    )
    with _lock:
        _write_file(cfg)
        _cache = cfg
    print(
        f"[llm] updated base_url={cfg['base_url']} model={cfg['model']} "
        f"key={'set' if cfg['api_key'] else 'empty'}"
    )
    return dict(cfg)


def llm_settings_tuple() -> tuple[str, str, str]:
    cfg = load_llm_config()
    return cfg["base_url"], cfg["api_key"], cfg["model"]

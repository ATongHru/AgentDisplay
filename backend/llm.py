"""OpenAI-compatible chat client (2api.store / TokenShop)."""

from __future__ import annotations

import json
import os
import time
from pathlib import Path
from typing import Any, Callable

try:
    from dotenv import load_dotenv
    load_dotenv(Path(__file__).with_name(".env"))
except ImportError:
    pass

try:
    from llm_config import load_llm_config
    load_llm_config()
except Exception as _exc:  # noqa: BLE001
    print(f"[llm] config load skipped: {_exc}")


def _settings() -> tuple[str, str, str]:
    from llm_config import llm_settings_tuple
    return llm_settings_tuple()


def chat_completions_url(base: str) -> str:
    if base.endswith("/chat/completions"):
        return base
    if base.endswith("/v1"):
        return f"{base}/chat/completions"
    return f"{base}/v1/chat/completions"


def llm_configured() -> bool:
    _base, key, _model = _settings()
    return bool(key)


def _delta_text(data: dict) -> str:
    choices = data.get("choices") or []
    if not choices:
        return ""
    choice = choices[0]
    delta = choice.get("delta") or {}
    if isinstance(delta, dict) and delta.get("content"):
        return str(delta["content"])
    message = choice.get("message") or {}
    if isinstance(message, dict) and message.get("content"):
        return str(message["content"])
    return ""


def _normalize_usage(raw: Any) -> dict[str, int]:
    if not isinstance(raw, dict):
        return {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0}
    prompt = int(raw.get("prompt_tokens") or raw.get("input_tokens") or 0)
    completion = int(raw.get("completion_tokens") or raw.get("output_tokens") or 0)
    total = int(raw.get("total_tokens") or (prompt + completion))
    return {"prompt_tokens": prompt, "completion_tokens": completion, "total_tokens": total}

async def _consume_response(response, *, on_partial, model: str, t0: float) -> dict[str, Any]:
    full = ""
    usage = _normalize_usage(None)
    ctype = response.headers.get("content-type", "")
    if "text/event-stream" in ctype or "octet-stream" in ctype:
        async for line in response.aiter_lines():
            if not line:
                continue
            if line.startswith(":"):
                continue
            if line.startswith("data:"):
                line = line[5:].strip()
            if line == "[DONE]":
                break
            try:
                data = json.loads(line)
            except json.JSONDecodeError:
                continue
            if data.get("error"):
                raise RuntimeError(str(data["error"]))
            if data.get("usage"):
                usage = _normalize_usage(data.get("usage"))
            piece = _delta_text(data)
            if not piece:
                continue
            full += piece
            if on_partial:
                on_partial(full)
    else:
        raw = await response.aread()
        data = json.loads(raw.decode("utf-8"))
        if data.get("error"):
            raise RuntimeError(str(data["error"]))
        if data.get("usage"):
            usage = _normalize_usage(data.get("usage"))
        full = _delta_text(data)
        if on_partial and full:
            on_partial(full)
    latency_ms = int((time.monotonic() - t0) * 1000)
    return {"text": full.strip(), "model": model, "usage": usage, "latency_ms": latency_ms}


async def llm_chat(
    user_text: str,
    on_partial: Callable[[str], None] | None = None,
    history: list[dict[str, str]] | None = None,
) -> dict[str, Any]:
    import httpx

    base, key, model = _settings()
    if not key:
        raise RuntimeError("LLM_API_KEY is empty")

    url = chat_completions_url(base)
    headers = {"Authorization": f"Bearer {key}", "Content-Type": "application/json"}
    messages: list[dict[str, str]] = [{
        "role": "system",
        "content": "你是米思林智能助手小E，尽量满足用户的需求和提问。用简体中文简短回答，不要用 Markdown，不要返回 emoji。",
    }]
    if history:
        for item in history:
            role = item.get("role")
            content = (item.get("content") or "").strip()
            if role in {"user", "assistant"} and content:
                messages.append({"role": role, "content": content})
    messages.append({"role": "user", "content": user_text})

    payload: dict[str, Any] = {
        "model": model,
        "messages": messages,
        "stream": True,
        "temperature": 0.7,
        "stream_options": {"include_usage": True},
    }

    t0 = time.monotonic()
    async with httpx.AsyncClient(timeout=120.0) as client:
        async with client.stream("POST", url, headers=headers, json=payload) as response:
            if response.status_code >= 400:
                body = (await response.aread()).decode("utf-8", errors="replace")
                if response.status_code == 400 and "stream_options" in body.lower():
                    payload.pop("stream_options", None)
                    async with client.stream("POST", url, headers=headers, json=payload) as response2:
                        if response2.status_code >= 400:
                            body2 = (await response2.aread()).decode("utf-8", errors="replace")
                            raise RuntimeError(f"LLM HTTP {response2.status_code}: {body2[:500]}")
                        return await _consume_response(response2, on_partial=on_partial, model=model, t0=t0)
                raise RuntimeError(f"LLM HTTP {response.status_code}: {body[:500]}")
            return await _consume_response(response, on_partial=on_partial, model=model, t0=t0)

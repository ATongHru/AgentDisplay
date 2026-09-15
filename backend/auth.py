"""Optional Bearer token auth for HTTP API and WebSocket."""

from __future__ import annotations

import os
from typing import Iterable

from fastapi import HTTPException, Request, WebSocket
from fastapi.responses import JSONResponse
from starlette.middleware.base import BaseHTTPMiddleware

API_TOKEN = os.getenv("AGENT_API_TOKEN", "").strip()
BIND_HOST = os.getenv("AGENT_BIND_HOST", "127.0.0.1").strip() or "127.0.0.1"

_PROTECTED_PREFIXES = ("/api/", "/event", "/voice/")


def auth_enabled() -> bool:
    return bool(API_TOKEN)


def bind_host() -> str:
    return BIND_HOST


def is_local_bind() -> bool:
    return BIND_HOST in {"127.0.0.1", "localhost", "::1"}


def _path_requires_auth(path: str) -> bool:
    return any(path.startswith(prefix) for prefix in _PROTECTED_PREFIXES)


def _extract_bearer(request: Request) -> str:
    header = request.headers.get("authorization") or ""
    if header.lower().startswith("bearer "):
        return header[7:].strip()
    return request.headers.get("x-api-token", "").strip()


def verify_token(token: str | None) -> bool:
    if not auth_enabled():
        return True
    return bool(token) and token == API_TOKEN


def require_http_token(request: Request) -> None:
    if not auth_enabled():
        return
    if not verify_token(_extract_bearer(request)):
        raise HTTPException(status_code=401, detail="Unauthorized")


def require_ws_token(websocket: WebSocket) -> None:
    if not auth_enabled():
        return
    token = websocket.query_params.get("token") or websocket.headers.get("x-api-token")
    if not verify_token(token):
        raise HTTPException(status_code=401, detail="Unauthorized")


class AuthMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request: Request, call_next):
        if auth_enabled() and _path_requires_auth(request.url.path):
            if not verify_token(_extract_bearer(request)):
                return JSONResponse({"detail": "Unauthorized"}, status_code=401)
        return await call_next(request)


def startup_warnings() -> list[str]:
    warnings: list[str] = []
    if not is_local_bind() and not auth_enabled():
        warnings.append(
            f"AGENT_BIND_HOST={BIND_HOST} 但未设置 AGENT_API_TOKEN；"
            "局域网暴露时建议同时配置鉴权令牌。"
        )
    return warnings

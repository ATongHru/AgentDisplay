"""Unified WebSocket hub for agent status + voice on a single /ws endpoint."""

from __future__ import annotations

import asyncio
import json
import threading
import time
from dataclasses import dataclass, field


AUDIO_CHUNK_BYTES = 1024
AUDIO_SAMPLE_RATE = 16000
AUDIO_BYTES_PER_SAMPLE = 2
AUDIO_SEND_INTERVAL_SEC = AUDIO_CHUNK_BYTES / (AUDIO_SAMPLE_RATE * AUDIO_BYTES_PER_SAMPLE)
from typing import Any, Callable

from fastapi import WebSocket, WebSocketDisconnect

from voice_pipeline import start_pipeline
from voice_session import session_store


@dataclass
class DeviceState:
    websocket: WebSocket | None = None
    waiting_audio: dict[str, Any] | None = None
    connected_at: float = 0.0
    last_seen: float = 0.0
    rssi: int | None = None


class WsHub:
    def __init__(
        self,
        push_status: Callable[[str, str, str], None],
        on_transcript: Callable[[str, str], None] | None = None,
    ):
        self._push_status = push_status
        self._on_transcript = on_transcript
        self._on_device_connect: Callable[[WsHub], Any] | None = None
        self._device = DeviceState()
        self._dashboards: set[WebSocket] = set()
        self._lock = asyncio.Lock()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._pending_audio: dict[str, list[tuple[bytes, bool]]] = {}
        self._session_device: dict[str, str] = {}

    def set_on_device_connect(self, callback: Callable[[WsHub], Any]) -> None:
        self._on_device_connect = callback

    def bind_loop(self, loop: asyncio.AbstractEventLoop) -> None:
        self._loop = loop

    @property
    def loop(self) -> asyncio.AbstractEventLoop | None:
        return self._loop

    @property
    def device_connected(self) -> bool:
        return self._device.websocket is not None

    @property
    def device_last_seen(self) -> float:
        return self._device.last_seen

    def device_snapshot(self) -> dict[str, Any]:
        return {
            "connected": self.device_connected,
            "connected_at": self._device.connected_at,
            "last_seen": self._device.last_seen,
            "rssi": self._device.rssi,
        }

    async def connect(self, websocket: WebSocket, role: str) -> None:
        if role == "device":
            old_websocket = None
            async with self._lock:
                old_websocket = self._device.websocket
                self._device.websocket = websocket
                self._device.connected_at = time.time()
                self._device.last_seen = time.time()
            if old_websocket is not None and old_websocket is not websocket:
                try:
                    await old_websocket.close(code=1000)
                except RuntimeError:
                    pass
            await websocket.send_json(
                {
                    "type": "ack",
                    "role": "device",
                    "server_time": int(time.time()),
                }
            )
            if self._on_device_connect:
                await self._on_device_connect(self)
            return
        self._dashboards.add(websocket)
        await websocket.send_json({"type": "ack", "role": role, "server_time": int(time.time())})

    async def disconnect(self, websocket: WebSocket) -> None:
        async with self._lock:
            if self._device.websocket is websocket:
                self._device.websocket = None
                self._device.waiting_audio = None
            self._dashboards.discard(websocket)

    async def handle_text(self, websocket: WebSocket, raw: str) -> None:
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            await websocket.send_json({"type": "error", "detail": "invalid json"})
            return

        msg_type = msg.get("type")
        if msg_type == "ping":
            if websocket is self._device.websocket:
                self._device.last_seen = time.time()
            await websocket.send_json({"type": "pong", "server_time": int(time.time())})
            return

        if msg_type == "hello":
            role = str(msg.get("role", "device"))
            if role == "device" and msg.get("rssi") is not None:
                self._device.rssi = int(msg["rssi"])
            await websocket.send_json({"type": "ack", "role": role, "server_time": int(time.time())})
            return

        if msg_type == "audio_upload":
            if websocket is not self._device.websocket:
                return
            self._device.waiting_audio = msg
            self._device.last_seen = time.time()
            return

        if msg_type == "status":
            await self._broadcast_dashboard(msg)
            return

    async def handle_binary(self, websocket: WebSocket, data: bytes) -> None:
        if websocket is not self._device.websocket:
            return
        self._device.last_seen = time.time()
        meta = self._device.waiting_audio
        self._device.waiting_audio = None
        if not meta:
            await websocket.send_json({"type": "error", "detail": "unexpected binary frame"})
            return

        expected = int(meta.get("audio_len", len(data)))
        if expected and len(data) != expected:
            await websocket.send_json(
                {
                    "type": "error",
                    "detail": f"audio_len mismatch: expected {expected}, got {len(data)}",
                }
            )
            return

        session = session_store.create(
            data,
            sample_rate=int(meta.get("sample_rate", 16000)),
            channels=int(meta.get("channels", 1)),
            bit_depth=int(meta.get("bit_depth", 16)),
        )
        self._session_device[session.session_id] = "device"
        await websocket.send_json({"type": "session", "session_id": session.session_id})
        self._push_status_threadsafe("THINKING", "正在处理…", "VOICE")
        start_pipeline(session.session_id, self._push_status_threadsafe, self._transcript_threadsafe)

    def _transcript_threadsafe(self, session_id: str, text: str, role: str = "user") -> None:
        if self._on_transcript is None:
            return
        if self._loop is None:
            self._on_transcript(session_id, text, role)
            return
        self._loop.call_soon_threadsafe(self._on_transcript, session_id, text, role)

    def _push_status_threadsafe(self, status: str, text: str, source: str = "VOICE") -> None:
        if self._loop is None:
            self._push_status(status, text, source)
            return
        self._loop.call_soon_threadsafe(self._push_status, status, text, source)

    async def push_status_event(self, payload: dict[str, Any]) -> None:
        # Keep text in the same frame so status/source/text cannot be reordered or dropped.
        message = {"type": "status", **payload}
        await self._send_device(message)
        await self._broadcast_dashboard(message)

    async def push_text_event(self, payload: dict[str, Any]) -> None:
        message = {"type": "text", **payload}
        await self._send_device(message)
        await self._broadcast_dashboard(message)

    async def push_audio_chunk(self, session_id: str, chunk: bytes, is_end: bool) -> None:
        device = self._device.websocket
        if device is None:
            return
        await device.send_json(
            {
                "type": "audio_chunk",
                "session_id": session_id,
                "len": len(chunk),
                "end": is_end,
            }
        )
        if chunk:
            await device.send_bytes(chunk)
        if is_end:
            self._push_status_threadsafe("IDLE", "", "VOICE")

    def schedule_audio_chunk(self, session_id: str, chunk: bytes, is_end: bool) -> None:
        if self._loop is None:
            return
        asyncio.run_coroutine_threadsafe(
            self.push_audio_chunk(session_id, chunk, is_end),
            self._loop,
        )

    async def _send_device(self, message: dict[str, Any]) -> None:
        device = self._device.websocket
        if device is None:
            return
        try:
            await device.send_json(message)
        except RuntimeError:
            pass

    async def _broadcast_dashboard(self, message: dict[str, Any]) -> None:
        dead: list[WebSocket] = []
        for client in list(self._dashboards):
            try:
                await client.send_json(message)
            except RuntimeError:
                dead.append(client)
        for client in dead:
            self._dashboards.discard(client)

    async def run_session_pump(self, stop_event: threading.Event) -> None:
        while not stop_event.is_set():
            for session_id in list(self._session_device.keys()):
                session = session_store.get(session_id)
                if session is None:
                    self._session_device.pop(session_id, None)
                    continue
                while True:
                    chunk, is_end = session.pop_audio_chunk()
                    if chunk is None and not is_end:
                        break
                    if chunk is None and is_end:
                        await self.push_audio_chunk(session_id, b"", True)
                        break
                    await self.push_audio_chunk(session_id, chunk, is_end)
                    if is_end:
                        break
                    # Pace delivery near real-time so the ESP32 ring buffer does not overflow.
                    await asyncio.sleep(AUDIO_SEND_INTERVAL_SEC)
            await asyncio.sleep(0.02)


async def ws_endpoint(hub: WsHub, websocket: WebSocket) -> None:
    await websocket.accept()
    role = "device"
    try:
        first = await websocket.receive_text()
        hello = json.loads(first)
        role = str(hello.get("role", "device"))
    except (json.JSONDecodeError, WebSocketDisconnect, RuntimeError):
        await websocket.close()
        return

    await hub.connect(websocket, role)
    try:
        while True:
            message = await websocket.receive()
            if message.get("type") == "websocket.disconnect":
                break
            if "text" in message:
                await hub.handle_text(websocket, message["text"])
            elif "bytes" in message:
                await hub.handle_binary(websocket, message["bytes"])
    except WebSocketDisconnect:
        pass
    finally:
        await hub.disconnect(websocket)

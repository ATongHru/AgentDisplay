"""Unified WebSocket hub for agent status + voice on a single /ws endpoint."""

from __future__ import annotations

import asyncio
import json
import threading
import time
from dataclasses import dataclass, field


AUDIO_CHUNK_BYTES = 4096
AUDIO_SAMPLE_RATE = 16000
AUDIO_BYTES_PER_SAMPLE = 2
AUDIO_SEND_INTERVAL_SEC = AUDIO_CHUNK_BYTES / (AUDIO_SAMPLE_RATE * AUDIO_BYTES_PER_SAMPLE)
AUDIO_BURST_BYTES = 16384
VOICE_BUSY_TIMEOUT_SEC = 45.0
from typing import Any, Callable

from fastapi import WebSocket, WebSocketDisconnect

from voice_pipeline import start_pipeline, tts_to_pcm
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
        self._device_send_lock = asyncio.Lock()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._pending_audio: dict[str, list[tuple[bytes, bool]]] = {}
        self._session_device: dict[str, str] = {}
        self._voice_busy = False
        self._busy_since = 0.0
        self._speak_sessions: set[str] = set()
        self._push_append: Callable[..., None] | None = None

    def set_push_append(self, callback: Callable[..., None] | None) -> None:
        self._push_append = callback

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
                except Exception:
                    pass
            await self._send_ws_json(
                websocket,
                {
                    "type": "ack",
                    "role": "device",
                    "server_time": int(time.time()),
                },
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
                self._speak_sessions.clear()
                self._clear_busy("device disconnect")
            self._dashboards.discard(websocket)

    async def handle_text(self, websocket: WebSocket, raw: str) -> None:
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            await self._send_ws_json(websocket, {"type": "error", "detail": "invalid json"})
            return

        msg_type = msg.get("type")
        if msg_type == "ping":
            if websocket is self._device.websocket:
                self._device.last_seen = time.time()
            await self._send_ws_json(websocket, {"type": "pong", "server_time": int(time.time())})
            return

        if msg_type == "hello":
            role = str(msg.get("role", "device"))
            if role == "device" and msg.get("rssi") is not None:
                self._device.rssi = int(msg["rssi"])
            await self._send_ws_json(
                websocket, {"type": "ack", "role": role, "server_time": int(time.time())}
            )
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
            await self._send_ws_json(websocket, {"type": "error", "detail": "unexpected binary frame"})
            return

        expected = int(meta.get("audio_len", len(data)))
        if expected and len(data) != expected:
            await self._send_ws_json(
                websocket,
                {
                    "type": "error",
                    "detail": f"audio_len mismatch: expected {expected}, got {len(data)}",
                },
            )
            return

        if self._voice_busy:
            print("[voice] drop upload, previous session still running")
            await self._send_ws_json(websocket, {"type": "error", "detail": "voice pipeline busy"})
            return

        session = session_store.create(
            data,
            sample_rate=int(meta.get("sample_rate", 16000)),
            channels=int(meta.get("channels", 1)),
            bit_depth=int(meta.get("bit_depth", 16)),
        )
        self._session_device[session.session_id] = "device"
        self._mark_busy()
        await self._send_ws_json(websocket, {"type": "session", "session_id": session.session_id})
        self._push_status_threadsafe("THINKING", "正在处理…", "VOICE")
        start_pipeline(
            session.session_id,
            self._push_status_threadsafe,
            self._transcript_threadsafe,
            self._on_pipeline_done,
            self._push_append_threadsafe,
        )

    def _mark_busy(self) -> None:
        self._voice_busy = True
        self._busy_since = time.time()

    def _clear_busy(self, reason: str) -> None:
        was_busy = self._voice_busy
        self._voice_busy = False
        self._busy_since = 0.0
        if was_busy:
            print(f"[voice] idle ({reason})")

    def _abort_speak(self, session_id: str, reason: str) -> None:
        self._session_device.pop(session_id, None)
        if session_id in self._speak_sessions:
            self._speak_sessions.discard(session_id)
            print(f"[tts] speak abort session={session_id} {reason}")
        self._clear_busy(reason)

    def _on_pipeline_done(self) -> None:
        if self._loop is not None:
            self._loop.call_soon_threadsafe(self._finish_pipeline_busy)
        else:
            self._finish_pipeline_busy()

    def _finish_pipeline_busy(self) -> None:
        if self._speak_sessions:
            print("[voice] pipeline finished, speak still pumping")
            return
        for session_id in list(self._session_device):
            session = session_store.get(session_id)
            if session is not None and session.audio_chunks:
                print("[voice] pipeline finished, audio still queued")
                return
        self._clear_busy("pipeline done")

    def _transcript_threadsafe(self, session_id: str, text: str, role: str = "user") -> None:
        if self._on_transcript is None:
            return
        if self._loop is None:
            self._on_transcript(session_id, text, role)
            return
        self._loop.call_soon_threadsafe(self._on_transcript, session_id, text, role)

    def _push_append_threadsafe(self, text: str, source: str = "VOICE", reset: bool = False) -> None:
        if self._push_append is None:
            return
        if self._loop is None:
            self._push_append(text, source, reset)
            return
        self._loop.call_soon_threadsafe(self._push_append, text, source, reset)

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

    async def push_append_event(self, payload: dict[str, Any]) -> None:
        message = {"type": "append", **payload}
        await self._send_device(message)

    async def push_text_event(self, payload: dict[str, Any]) -> None:
        message = {"type": "text", **payload}
        await self._send_device(message)
        await self._broadcast_dashboard(message)

    async def speak_text(self, text: str) -> dict[str, Any]:
        clean = (text or "").strip()
        if not clean:
            return {"ok": False, "detail": "empty text"}
        if self._device.websocket is None:
            return {"ok": False, "detail": "device offline"}
        if self._voice_busy:
            print("[tts] drop speak, voice pipeline busy")
            return {"ok": False, "detail": "voice busy"}
        self._mark_busy()
        try:
            pcm = await tts_to_pcm(clean)
            if not pcm:
                self._clear_busy("tts empty")
                return {"ok": False, "detail": "tts empty"}
            session = session_store.create(b"", sample_rate=16000, channels=1, bit_depth=16)
            session.set_tts_pcm(pcm)
            self._session_device[session.session_id] = "device"
            self._speak_sessions.add(session.session_id)
            print(
                f"[tts] speak session={session.session_id} bytes={len(pcm)} "
                f"chunks={len(session.audio_chunks)}"
            )
            return {"ok": True, "session_id": session.session_id, "bytes": len(pcm)}
        except Exception as exc:
            self._clear_busy(f"tts failed: {exc}")
            print(f"[tts] speak failed: {exc}")
            return {"ok": False, "detail": str(exc)}

    async def push_audio_chunk(self, session_id: str, chunk: bytes, is_end: bool) -> None:
        device = self._device.websocket
        if device is None:
            self._abort_speak(session_id, "device offline")
            return
        header = {
            "type": "audio_chunk",
            "session_id": session_id,
            "len": len(chunk),
            "end": is_end,
        }
        try:
            async with self._device_send_lock:
                await asyncio.wait_for(device.send_json(header), timeout=5)
                if chunk:
                    await asyncio.wait_for(device.send_bytes(chunk), timeout=8)
        except Exception as exc:
            self._abort_speak(session_id, f"send failed: {type(exc).__name__}: {exc}")
            return
        if is_end:
            self._session_device.pop(session_id, None)
            if session_id in self._speak_sessions:
                self._speak_sessions.discard(session_id)
                self._clear_busy("speak done")
                print(f"[tts] speak done session={session_id}")
            elif chunk:
                self._push_status_threadsafe("IDLE", "", "VOICE")
                self._clear_busy("voice audio done")

    def schedule_audio_chunk(self, session_id: str, chunk: bytes, is_end: bool) -> None:
        if self._loop is None:
            return
        asyncio.run_coroutine_threadsafe(
            self.push_audio_chunk(session_id, chunk, is_end),
            self._loop,
        )

    async def _send_ws_json(self, websocket: WebSocket, message: dict[str, Any]) -> None:
        try:
            async with self._device_send_lock:
                await asyncio.wait_for(websocket.send_json(message), timeout=5)
        except Exception as exc:
            print(f"[ws] send_json failed: {type(exc).__name__}: {exc}")

    async def _send_device(self, message: dict[str, Any]) -> None:
        device = self._device.websocket
        if device is None:
            return
        await self._send_ws_json(device, message)

    async def _broadcast_dashboard(self, message: dict[str, Any]) -> None:
        dead: list[WebSocket] = []
        for client in list(self._dashboards):
            try:
                await client.send_json(message)
            except Exception:
                dead.append(client)
        for client in dead:
            self._dashboards.discard(client)

    async def run_session_pump(self, stop_event: threading.Event) -> None:
        while not stop_event.is_set():
            try:
                if (
                    self._voice_busy
                    and self._busy_since
                    and (time.time() - self._busy_since) > VOICE_BUSY_TIMEOUT_SEC
                ):
                    stuck = list(self._speak_sessions) or list(self._session_device)
                    print(f"[voice] busy timeout sessions={stuck}")
                    self._speak_sessions.clear()
                    self._session_device.clear()
                    self._clear_busy("timeout")
                for session_id in list(self._session_device.keys()):
                    session = session_store.get(session_id)
                    if session is None:
                        self._abort_speak(session_id, "session missing")
                        continue
                    burst_sent = 0
                    sent_any = False
                    while True:
                        chunk, is_end = session.pop_audio_chunk()
                        if chunk is None and not is_end:
                            break
                        if chunk is None and is_end:
                            await self.push_audio_chunk(session_id, b"", True)
                            break
                        if not sent_any:
                            print(
                                f"[tts] pump start session={session_id} "
                                f"chunk={len(chunk)} end={is_end}"
                            )
                            sent_any = True
                        await self.push_audio_chunk(session_id, chunk, is_end)
                        burst_sent += len(chunk or b"")
                        if is_end:
                            break
                        if burst_sent >= AUDIO_BURST_BYTES:
                            await asyncio.sleep(AUDIO_SEND_INTERVAL_SEC)
            except Exception as exc:
                import traceback

                print(f"[pump] crashed: {type(exc).__name__}: {exc}")
                traceback.print_exc()
                self._speak_sessions.clear()
                self._clear_busy("pump error")
            await asyncio.sleep(0.02)


async def ws_endpoint(hub: WsHub, websocket: WebSocket) -> None:
    await websocket.accept()
    role = "device"
    try:
        first = await websocket.receive_text()
        hello = json.loads(first)
        role = str(hello.get("role", "device"))
    except (json.JSONDecodeError, WebSocketDisconnect, RuntimeError):
        try:
            await websocket.close()
        except Exception:
            pass
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
    except Exception as exc:
        print(f"[ws] {role} closed ({type(exc).__name__})")
    finally:
        await hub.disconnect(websocket)

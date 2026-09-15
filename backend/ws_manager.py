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
AUDIO_SEND_INTERVAL_SEC = 0.02
AUDIO_BURST_BYTES = 16384
VOICE_BUSY_TIMEOUT_SEC = 55.0
DEVICE_PING_SEC = 2.0
DEVICE_STALE_SEC = 30.0
from typing import Any, Callable

from fastapi import WebSocket, WebSocketDisconnect

from asr import VoskStreamRecognizer
from settings_store import get_volume_percent, get_voice_enabled
from voice_pipeline import cancel_all_pipelines, cancel_pipeline, start_pipeline, synthesize_to_session, tts_to_pcm
from voice_session import session_store


@dataclass
class DeviceState:
    websocket: WebSocket | None = None
    waiting_audio: dict[str, Any] | None = None
    stream_session_id: str | None = None
    stream_asr: Any = None
    connected_at: float = 0.0
    last_seen: float = 0.0
    rssi: int | None = None


class WsHub:
    def __init__(
        self,
        push_status: Callable[..., None],
        on_transcript: Callable[[str, str], None] | None = None,
    ):
        self._push_status = push_status
        self._on_transcript = on_transcript
        self._on_device_connect: Callable[[WsHub], Any] | None = None
        self._on_device_disconnect: Callable[[], None] | None = None
        self._on_device_display: Callable[[str, str], Any] | None = None
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
        self._profiles_future: asyncio.Future | None = None
        self._last_asr_partial_ts = 0.0
        self._last_device_ping = 0.0

    def set_push_append(self, callback: Callable[..., None] | None) -> None:
        self._push_append = callback

    def set_on_device_connect(self, callback: Callable[[WsHub], Any]) -> None:
        self._on_device_connect = callback

    def set_on_device_disconnect(self, callback: Callable[[], None] | None) -> None:
        self._on_device_disconnect = callback

    def set_on_device_display(self, callback: Callable[[str, str], Any] | None) -> None:
        self._on_device_display = callback

    def bind_loop(self, loop: asyncio.AbstractEventLoop) -> None:
        self._loop = loop

    @property
    def loop(self) -> asyncio.AbstractEventLoop | None:
        return self._loop

    def _device_is_live(self) -> bool:
        if self._device.websocket is None:
            return False
        seen = self._device.last_seen
        if seen <= 0:
            return False
        return (time.time() - seen) <= DEVICE_STALE_SEC

    @property
    def device_connected(self) -> bool:
        return self._device_is_live()

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
            await self.push_volume_config()
            if self._on_device_connect:
                await self._on_device_connect(self)
            return
        self._dashboards.add(websocket)
        await websocket.send_json({"type": "ack", "role": role, "server_time": int(time.time())})

    async def disconnect(self, websocket: WebSocket) -> None:
        dropped_device = False
        async with self._lock:
            if self._device.websocket is websocket:
                dropped_device = True
                self._device.websocket = None
                self._device.waiting_audio = None
                self._device.stream_session_id = None
                self._device.stream_asr = None
                self._speak_sessions.clear()
                self._clear_busy("device disconnect")
                fut = self._profiles_future
                if fut is not None and not fut.done():
                    fut.set_exception(RuntimeError("设备已断开"))
                self._profiles_future = None
            self._dashboards.discard(websocket)
        if dropped_device and self._on_device_disconnect:
            self._on_device_disconnect()
            await self._broadcast_dashboard({"type": "display", "status": "OFFLINE", "source": "BOT", "gif": "OFFLINE"})

    async def handle_text(self, websocket: WebSocket, raw: str) -> None:
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            await self._send_ws_json(websocket, {"type": "error", "detail": "invalid json"})
            return

        msg_type = msg.get("type")
        if websocket is self._device.websocket:
            self._device.last_seen = time.time()
        if msg_type == "ping":
            await self._send_ws_json(websocket, {"type": "pong", "server_time": int(time.time())})
            return
        if msg_type == "pong":
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
            self._device.last_seen = time.time()
            if not get_voice_enabled():
                await self._send_ws_json(websocket, {"type": "error", "detail": "voice chat disabled"})
                return
            streaming = bool(msg.get("stream")) or int(msg.get("audio_len", -1) or 0) == 0
            if streaming:
                await self._begin_audio_stream(websocket, msg)
            else:
                self._device.waiting_audio = msg
            return

        if msg_type == "audio_end":
            if websocket is not self._device.websocket:
                return
            self._device.last_seen = time.time()
            await self._finish_audio_stream(websocket, msg)
            return

        if msg_type == "display":
            if websocket is not self._device.websocket:
                return
            status = str(msg.get("status") or "").strip()
            source = str(msg.get("source") or "BOT").strip()
            snapshot = None
            if self._on_device_display:
                snapshot = self._on_device_display(status, source)
            payload = {"type": "display", **(snapshot or {"status": status, "source": source, "gif": status})}
            await self._broadcast_dashboard(payload)
            return

        if msg_type == "status":
            await self._broadcast_dashboard(msg)
            return

        if msg_type == "wifi_profiles":
            if websocket is not self._device.websocket:
                return
            fut = self._profiles_future
            if fut is not None and not fut.done():
                fut.set_result(msg)
            return

    async def handle_binary(self, websocket: WebSocket, data: bytes) -> None:
        if websocket is not self._device.websocket:
            return
        self._device.last_seen = time.time()

        if self._device.stream_session_id and self._device.stream_asr is not None:
            session = session_store.get(self._device.stream_session_id)
            if session is None:
                self._device.stream_session_id = None
                self._device.stream_asr = None
                await self._send_ws_json(websocket, {"type": "error", "detail": "stream session missing"})
                return
            session.append_pcm(data)
            try:
                partial = await asyncio.to_thread(self._device.stream_asr.accept, data)
            except (RuntimeError, OSError, ValueError) as exc:
                print(f"[asr] stream feed failed: {exc}")
                partial = ""
            if partial:
                print(f"[asr] partial={partial!r}")
                now = time.time()
                if now - self._last_asr_partial_ts >= 0.2:
                    self._last_asr_partial_ts = now
                    await self.push_append_event(
                        {"text": partial, "source": "VOICE", "reset": True, "role": "user"}
                    )
            return

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

        sample_rate = int(meta.get("sample_rate", 16000))
        channels = int(meta.get("channels", 1))
        bit_depth = int(meta.get("bit_depth", 16))
        session = session_store.create(
            data,
            sample_rate=sample_rate,
            channels=channels,
            bit_depth=bit_depth,
        )
        self._session_device[session.session_id] = "device"
        self._mark_busy()
        await self._send_ws_json(
            websocket,
            {
                "type": "session",
                "session_id": session.session_id,
                "volume_percent": get_volume_percent(),
            },
        )
        try:
            stream = VoskStreamRecognizer(sample_rate, channels, bit_depth)
            await asyncio.to_thread(stream.accept, data)
            text = await asyncio.to_thread(stream.finish)
            if text:
                session.mark_stream_asr(text)
        except (RuntimeError, OSError, ValueError) as exc:
            print(f"[asr] oneshot stream failed, fallback offline: {exc}")
        self._push_status_threadsafe("THINKING", "正在处理…", "VOICE")
        start_pipeline(
            session.session_id,
            self._push_status_threadsafe,
            self._transcript_threadsafe,
            self._on_pipeline_done,
            self._push_append_threadsafe,
        )

    async def _begin_audio_stream(self, websocket: WebSocket, meta: dict[str, Any]) -> None:
        if self._voice_busy or self._device.stream_session_id:
            print("[voice] drop stream start, busy")
            await self._send_ws_json(websocket, {"type": "error", "detail": "voice pipeline busy"})
            return
        sample_rate = int(meta.get("sample_rate", 16000))
        channels = int(meta.get("channels", 1))
        bit_depth = int(meta.get("bit_depth", 16))
        session = session_store.create(
            b"",
            sample_rate=sample_rate,
            channels=channels,
            bit_depth=bit_depth,
        )
        try:
            asr = VoskStreamRecognizer(sample_rate, channels, bit_depth)
        except Exception as exc:
            session_store.delete(session.session_id)
            await self._send_ws_json(websocket, {"type": "error", "detail": f"asr init failed: {exc}"})
            return
        self._session_device[session.session_id] = "device"
        self._device.stream_session_id = session.session_id
        self._device.stream_asr = asr
        self._mark_busy()
        await self._send_ws_json(
            websocket,
            {
                "type": "session",
                "session_id": session.session_id,
                "volume_percent": get_volume_percent(),
            },
        )
        print(f"[voice] stream start session={session.session_id}")

    async def _finish_audio_stream(self, websocket: WebSocket, msg: dict[str, Any]) -> None:
        session_id = self._device.stream_session_id or str(msg.get("session_id") or "")
        asr = self._device.stream_asr
        self._device.stream_session_id = None
        self._device.stream_asr = None
        if not session_id or asr is None:
            await self._send_ws_json(websocket, {"type": "error", "detail": "no active audio stream"})
            self._clear_busy("stream end without session")
            return
        session = session_store.get(session_id)
        if session is None:
            await self._send_ws_json(websocket, {"type": "error", "detail": "stream session missing"})
            self._clear_busy("stream session missing")
            return
        if msg.get("discard"):
            session.mark_done()
            self._clear_busy("stream discarded")
            print(f"[voice] stream discarded session={session_id}")
            return
        try:
            text = await asyncio.to_thread(asr.finish)
        except (RuntimeError, OSError, ValueError) as exc:
            print(f"[asr] stream finish failed: {exc}")
            text = ""
        pcm_len = len(session.pcm_data)
        expected = msg.get("total_bytes")
        if expected is not None:
            try:
                expected_n = int(expected)
            except (TypeError, ValueError):
                expected_n = -1
            if expected_n >= 0 and expected_n != pcm_len:
                print(f"[voice] total_bytes mismatch expect={expected_n} got={pcm_len}")
                await self._send_ws_json(
                    websocket,
                    {
                        "type": "error",
                        "detail": f"total_bytes mismatch: expected {expected_n}, got {pcm_len}",
                    },
                )
                session.mark_done()
                self._clear_busy("total_bytes mismatch")
                self._push_status_threadsafe("IDLE", "", "VOICE")
                return
        print(f"[voice] stream end session={session_id} pcm={pcm_len} text={text!r}")
        if pcm_len < 6400 or not text:
            session.mark_done()
            self._clear_busy("stream too short/empty")
            detail = "没听清，请再说一次" if pcm_len >= 6400 else ""
            self._push_status_threadsafe("IDLE", detail, "VOICE")
            return
        session.mark_stream_asr(text)
        self._push_status_threadsafe(
            "THINKING", text or "正在处理…", "VOICE", "user" if text else None
        )
        start_pipeline(
            session.session_id,
            self._push_status_threadsafe,
            self._transcript_threadsafe,
            self._on_pipeline_done,
            self._push_append_threadsafe,
        )

    async def push_volume_config(self, percent: int | None = None) -> None:
        """Sync dashboard volume / voice switch to device."""
        value = get_volume_percent() if percent is None else int(percent)
        enabled = get_voice_enabled()
        await self._send_device(
            {"type": "config", "volume_percent": value, "voice_enabled": enabled}
        )
        print(f"[volume] pushed to device {value}% voice_enabled={enabled}")

    async def push_voice_config(self, enabled: bool | None = None) -> None:
        value = get_voice_enabled() if enabled is None else bool(enabled)
        await self._send_device(
            {
                "type": "config",
                "volume_percent": get_volume_percent(),
                "voice_enabled": value,
            }
        )
        print(f"[voice] pushed enabled={value}")
        if not value:
            self._abort_listen_stream("voice disabled")

    async def request_wifi_profiles(self, timeout: float = 5.0) -> dict[str, Any]:
        return await self.request_wifi_command({"type": "get_wifi_profiles"}, timeout=timeout)

    async def request_wifi_command(self, message: dict[str, Any], timeout: float = 8.0) -> dict[str, Any]:
        """Send a WiFi-profile command and wait for the device wifi_profiles reply."""
        if self._device.websocket is None:
            raise RuntimeError("设备未连接 WebSocket，无法读写网络配置")
        loop = asyncio.get_running_loop()
        if self._profiles_future is not None and not self._profiles_future.done():
            raise RuntimeError("正在与设备交换网络配置")
        fut = loop.create_future()
        self._profiles_future = fut
        try:
            await self._send_device(message)
            msg = await asyncio.wait_for(asyncio.shield(fut), timeout)
        except asyncio.TimeoutError as exc:
            raise RuntimeError("设备未在超时内返回配置，请确认已烧录支持该接口的固件") from exc
        finally:
            if self._profiles_future is fut:
                self._profiles_future = None
        if msg.get("ok") is False:
            raise RuntimeError(str(msg.get("error") or "设备拒绝该操作"))
        profiles = msg.get("profiles") or []
        if not isinstance(profiles, list):
            profiles = []
        cleaned = [item for item in profiles if isinstance(item, dict)]
        return {
            "ok": True,
            "count": int(msg.get("count") or len(cleaned)),
            "active": int(msg.get("active") or 0),
            "profiles": cleaned,
        }

    def _abort_listen_stream(self, reason: str) -> None:
        session_id = self._device.stream_session_id
        self._device.stream_session_id = None
        self._device.stream_asr = None
        if not session_id:
            return
        session = session_store.get(session_id)
        if session is not None:
            session.mark_done()
        self._session_device.pop(session_id, None)
        print(f"[voice] abort listen session={session_id} {reason}")
        if session_id not in self._speak_sessions:
            self._clear_busy(reason)

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
            if session is not None and session.audio_still_active():
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

    def _push_append_threadsafe(
        self,
        text: str,
        source: str = "VOICE",
        reset: bool = False,
        role: str = "assistant",
    ) -> None:
        if self._push_append is None:
            return
        if self._loop is None:
            self._push_append(text, source, reset, role)
            return
        self._loop.call_soon_threadsafe(self._push_append, text, source, reset, role)

    def _push_status_threadsafe(
        self,
        status: str,
        text: str,
        source: str = "VOICE",
        role: str | None = None,
    ) -> None:
        if self._loop is None:
            self._push_status(status, text, source, role)
            return
        self._loop.call_soon_threadsafe(self._push_status, status, text, source, role)

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
        if self._voice_busy and self._device.stream_session_id and not self._speak_sessions:
            self._abort_listen_stream("preempt for speak")
        if self._voice_busy:
            print("[tts] drop speak, voice pipeline busy")
            return {"ok": False, "detail": "voice busy"}
        self._mark_busy()
        try:
            await self.push_volume_config()
            session = session_store.create(b"", sample_rate=16000, channels=1, bit_depth=16)
            self._session_device[session.session_id] = "device"
            self._speak_sessions.add(session.session_id)
            total = await synthesize_to_session(session, clean)
            if not total:
                self._session_device.pop(session.session_id, None)
                self._speak_sessions.discard(session.session_id)
                self._clear_busy("tts empty")
                return {"ok": False, "detail": "tts empty"}
            print(
                f"[tts] speak session={session.session_id} bytes={total} "
                f"chunks={len(session.audio_chunks)} (streamed)"
            )
            return {"ok": True, "session_id": session.session_id, "bytes": total}
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
            self._device.last_seen = time.time()
        except Exception as exc:
            self._abort_speak(session_id, f"send failed: {type(exc).__name__}: {exc}")
            return
        if is_end:
            self._session_device.pop(session_id, None)
            if session_id in self._speak_sessions:
                self._speak_sessions.discard(session_id)
                self._clear_busy("speak done")
                print(f"[tts] speak done session={session_id}")
            else:
                self._clear_busy("voice audio done")
                # Do not push VOICE IDLE here: the audio_chunk end flag is enough.
                # A status IDLE during PLAYING used to fade remaining TTS to silence.

    def schedule_audio_chunk(self, session_id: str, chunk: bytes, is_end: bool) -> None:
        if self._loop is None:
            return
        asyncio.run_coroutine_threadsafe(
            self.push_audio_chunk(session_id, chunk, is_end),
            self._loop,
        )

    async def _ping_device_if_needed(self) -> None:
        ws = self._device.websocket
        if ws is None:
            return
        now = time.time()
        if now - self._last_device_ping < DEVICE_PING_SEC:
            return
        self._last_device_ping = now
        try:
            async with self._device_send_lock:
                await asyncio.wait_for(ws.send_json({"type": "ping"}), timeout=1.0)
        except Exception as exc:
            print(f"[ws] device ping failed: {type(exc).__name__}: {exc}")
            await self.disconnect(ws)

    async def _drop_stale_device(self) -> None:
        ws = self._device.websocket
        if ws is None:
            return
        if self._speak_sessions or self._voice_busy:
            return
        seen = self._device.last_seen
        if seen <= 0 or (time.time() - seen) <= DEVICE_STALE_SEC:
            return
        print("[ws] device stale, mark offline")
        await self.disconnect(ws)
        try:
            await ws.close(code=1001)
        except Exception:
            pass

    async def _send_ws_json(self, websocket: WebSocket, message: dict[str, Any]) -> None:
        try:
            async with self._device_send_lock:
                await asyncio.wait_for(websocket.send_json(message), timeout=5)
        except Exception as exc:
            print(f"[ws] send_json failed: {type(exc).__name__}: {exc}")
            if websocket is self._device.websocket:
                await self.disconnect(websocket)

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
                await self._ping_device_if_needed()
                await self._drop_stale_device()
                session_store.cleanup_expired()
                if (
                    self._voice_busy
                    and self._busy_since
                    and (time.time() - self._busy_since) > VOICE_BUSY_TIMEOUT_SEC
                ):
                    stuck = list(self._speak_sessions) or list(self._session_device)
                    print(f"[voice] busy timeout sessions={stuck}")
                    self._device.stream_session_id = None
                    self._device.stream_asr = None
                    for session_id in stuck:
                        cancel_pipeline(session_id)
                    cancel_all_pipelines()
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
    from auth import auth_enabled, verify_token

    if auth_enabled():
        token = websocket.query_params.get("token") or websocket.headers.get("x-api-token")
        if not verify_token(token):
            await websocket.close(code=1008)
            return

    await websocket.accept()
    role = "device"
    try:
        first = await websocket.receive_text()
        hello = json.loads(first)
        role = str(hello.get("role", "device"))
    except (json.JSONDecodeError, WebSocketDisconnect, RuntimeError):
        try:
            await websocket.close()
        except OSError:
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

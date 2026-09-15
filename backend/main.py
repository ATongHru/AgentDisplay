import asyncio
import json
import os
import threading
import time
from collections import deque
from contextlib import asynccontextmanager
from datetime import datetime
from pathlib import Path

try:
    from dotenv import load_dotenv
    load_dotenv(Path(__file__).with_name(".env"))
except ImportError:
    pass

from fastapi import FastAPI, HTTPException, Request, Response, WebSocket
from fastapi.responses import FileResponse, HTMLResponse
from pydantic import BaseModel, Field

from agent_status import STATUS_DETAILS, TASK_LABELS, TOOL_CATEGORIES, normalize_status
from log_hub import install_capture, log_hub
from llm_history import query_records
from settings_store import (
    get_volume_percent,
    set_volume_percent,
    get_voice_enabled,
    set_voice_enabled,
    get_tts_voice,
    set_tts_voice,
    list_tts_voices,
)
from wifi_profiles_store import (
    list_profiles as list_wifi_profiles,
    upsert_profile as upsert_wifi_profile,
    delete_profile as delete_wifi_profile,
    get_profile as get_wifi_profile,
)
from llm_config import (
    activate_llm_profile,
    delete_llm_profile,
    get_llm_config,
    upsert_llm_profile,
)
from auth import AuthMiddleware, auth_enabled, bind_host, startup_warnings
from hook_bridge import clear_hook_online, run_hook_loop, stop_hook_loop
from sanitize import mask_wifi_profile
from voice_pipeline import bind_loop as bind_voice_loop, start_pipeline, warmup as voice_warmup
from voice_session import session_store
from ble_prov import ble_prov
from ws_manager import WsHub, ws_endpoint

install_capture()

DASHBOARD_HTML_PATH = Path(__file__).with_name("dashboard.html")
CHAT_HISTORY_HTML_PATH = Path(__file__).with_name("chat_history.html")
DASHBOARD_HTML = DASHBOARD_HTML_PATH.read_text(encoding="utf-8") if DASHBOARD_HTML_PATH.is_file() else ""
CHAT_HISTORY_HTML = (
    CHAT_HISTORY_HTML_PATH.read_text(encoding="utf-8") if CHAT_HISTORY_HTML_PATH.is_file() else ""
)
GIF_DIR = Path(__file__).resolve().parent.parent / "third_party" / "emoji-gif"
GIF_STATUSES = {
    "IDLE", "THINKING", "CODING", "READING", "TESTING", "WAITING",
    "DONE", "ERROR", "OFFLINE", "STALE", "UNKNOWN",
    "TOOL", "EAR", "SPEAKING",
}


class Event(BaseModel):
    status: str = Field(min_length=1, max_length=32)
    text: str = Field(default="", max_length=240)
    status_detail: str | None = Field(default=None, max_length=32)
    tool_category: str | None = Field(default=None, max_length=16)
    task_label: str | None = Field(default=None, max_length=32)
    source: str = Field(default="unknown", max_length=16)

    def normalized(self):
        try:
            status, legacy_detail = normalize_status(self.status)
        except ValueError as exc:
            raise ValueError(str(exc)) from exc
        detail = self.status_detail or legacy_detail
        if detail is not None and detail not in STATUS_DETAILS:
            raise ValueError("unsupported status_detail")
        if self.tool_category is not None and self.tool_category not in TOOL_CATEGORIES:
            raise ValueError("unsupported tool_category")
        if self.task_label is not None and self.task_label not in TASK_LABELS:
            raise ValueError("unsupported task_label")
        source = self.source.upper()
        if source not in {"PI", "CURSOR", "UNKNOWN", "VOICE", "BOT"}:
            raise ValueError("unsupported source")
        return {
            "status": status,
            "text": self.text,
            "source": source,
            **({"status_detail": detail} if detail else {}),
            **({"tool_category": self.tool_category} if self.tool_category else {}),
            **({"task_label": self.task_label} if self.task_label else {}),
        }


class StatusEvent(BaseModel):
    status: str = Field(min_length=1, max_length=32)
    source: str = Field(default="BOT", max_length=16)
    status_detail: str | None = Field(default=None, max_length=32)
    tool_category: str | None = Field(default=None, max_length=16)
    task_label: str | None = Field(default=None, max_length=32)

    def normalized(self) -> dict:
        event = Event(
            status=self.status, text="", source=self.source,
            status_detail=self.status_detail, tool_category=self.tool_category,
            task_label=self.task_label,
        )
        data = event.normalized()
        return {k: v for k, v in data.items() if k != "text"}


class VolumeEvent(BaseModel):
    volume_percent: int = Field(ge=0, le=100)


class VoiceToggleEvent(BaseModel):
    voice_enabled: bool


class TtsVoiceEvent(BaseModel):
    tts_voice: str = Field(min_length=3, max_length=96)


class WifiProfileEvent(BaseModel):
    id: str | None = None
    name: str = ""
    ssid: str = Field(min_length=1, max_length=32)
    password: str = Field(default="", max_length=64)
    host: str = Field(min_length=1, max_length=64)
    port: int = Field(default=8000, ge=1, le=65535)
    ip: str = Field(default="", max_length=15)
    netmask: str = Field(default="", max_length=15)
    gateway: str = Field(default="", max_length=15)


class DeviceWifiProfileSave(BaseModel):
    index: int | None = Field(default=None, ge=-1, le=4)
    ssid: str = Field(min_length=1, max_length=32)
    password: str = Field(default="", max_length=64)
    host: str = Field(min_length=1, max_length=64)
    port: int = Field(default=8000, ge=1, le=65535)
    ip: str = Field(default="", max_length=15)
    netmask: str = Field(default="", max_length=15)
    gateway: str = Field(default="", max_length=15)


class DeviceWifiIndexEvent(BaseModel):
    index: int = Field(ge=0, le=4)


class LlmConfigEvent(BaseModel):
    id: str | None = Field(default=None, max_length=32)
    name: str = Field(default="", max_length=32)
    base_url: str = Field(min_length=1, max_length=256)
    api_key: str = Field(default="", max_length=256)
    model: str = Field(min_length=1, max_length=128)
    activate: bool = False


class LlmIdEvent(BaseModel):
    id: str = Field(min_length=1, max_length=32)


class BleScanRequest(BaseModel):
    timeout: float = Field(default=6.0, ge=2.0, le=20.0)


class BleReadRequest(BaseModel):
    address: str | None = Field(default=None, max_length=64)


class BleProvRequest(BaseModel):
    ssid: str = Field(min_length=1, max_length=32)
    password: str = Field(default="", max_length=64)
    host: str = Field(min_length=1, max_length=64)
    port: int = Field(default=8000, ge=1, le=65535)
    address: str | None = Field(default=None, max_length=64)
    api_token: str | None = Field(default=None, max_length=96)
    ip: str | None = Field(default=None, max_length=15)
    netmask: str | None = Field(default=None, max_length=15)
    gateway: str | None = Field(default=None, max_length=15)


class TextEvent(BaseModel):
    text: str = Field(default="", max_length=240)
    source: str = Field(default="VOICE", max_length=16)
    role: str | None = Field(default=None, max_length=16)
    speak: bool = False

    def normalized(self) -> dict:
        source = self.source.upper()
        if source not in {"PI", "CURSOR", "UNKNOWN", "VOICE", "BOT"}:
            raise ValueError("unsupported source")
        # Device captions only render source=VOICE. This endpoint is the bottom
        # ticker; other sources would silently disappear on the ESP.
        source = "VOICE"
        payload = {"text": self.text, "source": source}
        role = (self.role or "assistant").strip().lower()
        if role in {"user", "assistant"}:
            payload["role"] = role
        return payload


def _device_status_fingerprint(payload: dict) -> tuple:
    """ESP 侧关心的状态键；连续相同则不重复下发。"""
    return (
        payload.get("status"),
        payload.get("source"),
        payload.get("status_detail"),
        payload.get("tool_category"),
        payload.get("task_label"),
        payload.get("text") or "",
    )


class AppState:
    def __init__(self):
        self.lock = threading.Lock()
        self.current_event = {"status": "IDLE", "source": "BOT", "text": ""}
        self.history = deque(maxlen=50)
        self.transcripts = deque(maxlen=200)
        self.last_device_status_fp: tuple | None = None
        self.device_display: dict | None = None

    def record_history(self, snapshot: dict) -> None:
        self.history.appendleft({**snapshot, "time": datetime.now().strftime("%H:%M:%S")})
        self.current_event = {**self.current_event, **snapshot}

    def add_transcript(self, session_id: str, text: str, role: str = "user") -> dict:
        role = role if role in {"user", "assistant"} else "user"
        item = {
            "time": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "text": text.strip() if text and text.strip() else "（未识别到语音）",
            "session_id": session_id,
            "role": role,
        }
        self.transcripts.appendleft(item)
        return item


def apply_device_display(status: str, source: str) -> dict:
    status_key, _ = normalize_status(status)
    src = (source or "BOT").strip().upper() or "BOT"
    if src not in {"PI", "CURSOR", "UNKNOWN", "VOICE", "BOT"}:
        src = "BOT"
    snapshot = {
        "status": status_key,
        "source": src,
        "gif": status_key,
        "time": datetime.now().strftime("%H:%M:%S"),
    }
    with state.lock:
        prev = state.device_display or {}
        if prev.get("status") == snapshot["status"] and prev.get("source") == snapshot["source"]:
            return dict(prev)
        state.device_display = snapshot
    return snapshot


def clear_device_display() -> None:
    with state.lock:
        state.device_display = None


state = AppState()


def _push_voice_event(
    status: str, text: str, source: str = "VOICE", role: str | None = None
) -> None:
    """Voice captions only. Do not overwrite agent status/source/GIF."""
    if text and len(text) > 240:
        text = text[:237] + "..."
    if ws_hub.loop is None:
        return
    payload: dict = {"text": text or "", "source": source, "status": status}
    if role:
        payload["role"] = role
    asyncio.run_coroutine_threadsafe(ws_hub.push_text_event(payload), ws_hub.loop)


def _push_voice_append(
    text: str, source: str = "VOICE", reset: bool = False, role: str = "assistant"
) -> None:
    if ws_hub.loop is not None:
        asyncio.run_coroutine_threadsafe(
            ws_hub.push_append_event(
                {
                    "text": text or "",
                    "source": source,
                    "reset": bool(reset),
                    "role": role,
                }
            ),
            ws_hub.loop,
        )


def _on_transcript(session_id: str, text: str, role: str = "user") -> None:
    with state.lock:
        item = state.add_transcript(session_id, text, role)
    tag = "llm" if item.get("role") == "assistant" else "asr"
    print(f"[{tag}] {item['time']} {item['text']}")
    if ws_hub.loop is not None:
        asyncio.run_coroutine_threadsafe(
            ws_hub._broadcast_dashboard({"type": "transcript", **item}),
            ws_hub.loop,
        )


ws_hub = WsHub(_push_voice_event, _on_transcript)
ws_hub.set_push_append(_push_voice_append)


def _on_device_display(status: str, source: str) -> dict | None:
    try:
        return apply_device_display(status, source)
    except ValueError:
        print(f"[display] ignore invalid status={status!r}")
        return None


ws_hub.set_on_device_display(_on_device_display)
ws_hub.set_on_device_disconnect(clear_device_display)


def _on_backend_log(item: dict) -> None:
    if ws_hub.loop is None:
        return
    asyncio.run_coroutine_threadsafe(
        ws_hub._broadcast_dashboard({"type": "backend_log", **item}),
        ws_hub.loop,
    )


log_hub.set_listener(_on_backend_log)


def _queue_device_status(payload: dict, *, force: bool = False) -> bool:
    """下发状态到 ESP。连续相同指纹只保留第一次；返回是否实际发送。"""
    device_payload = dict(payload)
    if not device_payload.get("text"):
        device_payload.pop("text", None)
    fp = _device_status_fingerprint(device_payload)
    with state.lock:
        if not force and fp == state.last_device_status_fp:
            return False
        state.last_device_status_fp = fp
    if ws_hub.loop is not None:
        asyncio.run_coroutine_threadsafe(ws_hub.push_status_event(device_payload), ws_hub.loop)
    return True


def apply_event(event: Event) -> tuple[dict, bool]:
    normalized = event.normalized()
    device_payload = dict(normalized)
    if not device_payload.get("text"):
        device_payload.pop("text", None)
    fp = _device_status_fingerprint(device_payload)
    with state.lock:
        merged = {**state.current_event, **normalized}
        if not normalized.get("text"):
            merged["text"] = state.current_event.get("text", "")
        if fp == state.last_device_status_fp:
            return merged, False
        state.record_history(merged)
        state.last_device_status_fp = fp
    if ws_hub.loop is not None:
        asyncio.run_coroutine_threadsafe(ws_hub.push_status_event(device_payload), ws_hub.loop)
    return merged, True


async def _sync_device_state(hub: WsHub) -> None:
    with state.lock:
        event = dict(state.current_event or {})
        state.last_device_status_fp = None
    status_payload = {k: v for k, v in event.items() if k != "text"}
    _queue_device_status(status_payload or {"status": "IDLE", "source": "BOT"}, force=True)
    text = event.get("text") or ""
    if text:
        await hub.push_text_event({"text": text, "source": event.get("source") or "BOT"})


ws_hub.set_on_device_connect(_sync_device_state)

def _apply_hook_line(data: dict) -> None:
    apply_event(Event(**data))


@asynccontextmanager
async def lifespan(_: FastAPI):
    loop = asyncio.get_running_loop()
    ws_hub.bind_loop(loop)
    bind_voice_loop(loop)
    for warning in startup_warnings():
        print(f"[auth] {warning}")
    if auth_enabled():
        print("[auth] API token protection enabled")
    print(f"[server] bind host={bind_host()}")
    pump_stop = threading.Event()
    pump_task = asyncio.create_task(ws_hub.run_session_pump(pump_stop))
    await asyncio.to_thread(voice_warmup)
    hook_thread = threading.Thread(
        target=run_hook_loop,
        args=(_apply_hook_line,),
        daemon=True,
        name="hook-queue",
    )
    hook_thread.start()
    yield
    stop_hook_loop()
    clear_hook_online()
    pump_stop.set()
    pump_task.cancel()
    try:
        await pump_task
    except asyncio.CancelledError:
        pass


app = FastAPI(title="Agent 状态显示后端", lifespan=lifespan)
app.add_middleware(AuthMiddleware)


@app.get("/", response_class=HTMLResponse)
def dashboard():
    return HTMLResponse(DASHBOARD_HTML)


@app.get("/dashboard_i18n.js")
def dashboard_i18n_js():
    path = Path(__file__).with_name("dashboard_i18n.js")
    if not path.is_file():
        raise HTTPException(status_code=404, detail="dashboard_i18n.js missing")
    return HTMLResponse(path.read_text(encoding="utf-8"), media_type="application/javascript")


@app.get("/chat-history", response_class=HTMLResponse)
def chat_history_page():
    if not CHAT_HISTORY_HTML:
        raise HTTPException(status_code=404, detail="chat history page missing")
    return HTMLResponse(CHAT_HISTORY_HTML)


@app.get("/api/chat-history")
def api_chat_history(
    page: int = 1,
    page_size: int = 20,
    q: str = "",
    source: str = "",
    date_from: str = "",
    date_to: str = "",
):
    return query_records(
        page=page,
        page_size=page_size,
        q=q,
        source=source,
        date_from=date_from,
        date_to=date_to,
    )



@app.get("/gifs/{name}")
def status_gif(name: str):
    stem = Path(name).stem.upper()
    if stem not in GIF_STATUSES:
        stem = "UNKNOWN"
    path = GIF_DIR / f"{stem}.gif"
    if not path.is_file():
        raise HTTPException(status_code=404, detail="gif not found")
    return FileResponse(
        path,
        media_type="image/gif",
        headers={"Cache-Control": "public, max-age=86400"},
    )


@app.get("/api/status")
def api_status():
    with state.lock:
        current = dict(state.current_event)
        history = list(state.history)
        transcripts = list(state.transcripts)
        display = dict(state.device_display) if state.device_display else None
    device = ws_hub.device_snapshot()
    link_status = "CONNECTED" if device.get("connected") else "OFFLINE"
    return {
        "backend": "ONLINE",
        "usb": "UNUSED",
        "wifi": {
            "status": link_status,
            "target": os.getenv("ESP32_WS_URL", "ws://0.0.0.0:8000/ws"),
            "failures": 0,
            "device": device,
            "router_wifi": device.get("connected"),
        },
        "event": current,
        "display": display,
        "history": history,
        "transcripts": transcripts,
        "logs": log_hub.items(),
        "volume_percent": get_volume_percent(),
        "voice_enabled": get_voice_enabled(),
        "tts_voice": get_tts_voice(),
        "tts_voices": list_tts_voices(get_tts_voice()),
        "wifi_profiles": list_wifi_profiles(),
        "llm": get_llm_config(mask_key=True),
        "updated": time.time(),
    }


@app.get("/api/volume")
def api_volume_get():
    return {"volume_percent": get_volume_percent()}


@app.post("/api/volume")
async def api_volume_set(payload: VolumeEvent):
    percent = set_volume_percent(payload.volume_percent)
    await ws_hub.push_volume_config(percent)
    return {"ok": True, "volume_percent": percent}


@app.get("/api/voice")
def api_voice_get():
    return {"voice_enabled": get_voice_enabled()}


@app.post("/api/voice")
async def api_voice_set(payload: VoiceToggleEvent):
    enabled = set_voice_enabled(payload.voice_enabled)
    await ws_hub.push_voice_config(enabled)
    return {"ok": True, "voice_enabled": enabled}


@app.get("/api/tts-voice")
def api_tts_voice_get():
    voice = get_tts_voice()
    return {"tts_voice": voice, "voices": list_tts_voices(voice)}


@app.post("/api/tts-voice")
def api_tts_voice_set(payload: TtsVoiceEvent):
    voice = set_tts_voice(payload.tts_voice)
    return {"ok": True, "tts_voice": voice, "voices": list_tts_voices(voice)}



@app.get("/api/wifi-profiles")
def api_wifi_profiles_list():
    return {"profiles": list_wifi_profiles()}


@app.get("/api/wifi-profiles/{profile_id}")
def api_wifi_profile_get(profile_id: str, reveal: bool = False):
    item = get_wifi_profile(profile_id, reveal_password=reveal)
    if item is None:
        raise HTTPException(status_code=404, detail="profile not found")
    return {"profile": item}


@app.post("/api/wifi-profiles")
def api_wifi_profiles_upsert(payload: WifiProfileEvent):
    try:
        item = upsert_wifi_profile(payload.model_dump())
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    return {"ok": True, "profile": mask_wifi_profile(item), "profiles": list_wifi_profiles()}


@app.delete("/api/wifi-profiles/{profile_id}")
def api_wifi_profiles_delete(profile_id: str):
    ok = delete_wifi_profile(profile_id)
    if not ok:
        raise HTTPException(status_code=404, detail="profile not found")
    return {"ok": True, "profiles": list_wifi_profiles()}


@app.get("/api/llm")
def api_llm_get():
    return get_llm_config(mask_key=True)


@app.post("/api/llm")
def api_llm_set(payload: LlmConfigEvent):
    try:
        data = upsert_llm_profile(payload.model_dump(), activate=payload.activate)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except OSError as exc:
        raise HTTPException(status_code=500, detail=f"写入 llm.json 失败: {exc}") from exc
    return {"ok": True, "llm": data}


@app.post("/api/llm/activate")
def api_llm_activate(payload: LlmIdEvent):
    try:
        data = activate_llm_profile(payload.id)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except OSError as exc:
        raise HTTPException(status_code=500, detail=f"写入 llm.json 失败: {exc}") from exc
    return {"ok": True, "llm": data}


@app.post("/api/llm/delete")
def api_llm_delete(payload: LlmIdEvent):
    try:
        data = delete_llm_profile(payload.id)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except OSError as exc:
        raise HTTPException(status_code=500, detail=f"写入 llm.json 失败: {exc}") from exc
    return {"ok": True, "llm": data}


@app.get("/api/ble/status")
def api_ble_status():
    return ble_prov.status()


@app.post("/api/ble/scan")
async def api_ble_scan(payload: BleScanRequest = BleScanRequest()):
    timeout = payload.timeout
    try:
        devices = await ble_prov.scan(timeout=timeout)
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    return {"ok": True, "devices": devices, **ble_prov.status()}


@app.post("/api/ble/provision")
async def api_ble_provision(payload: BleProvRequest):
    try:
        result = await ble_prov.provision(
            ssid=payload.ssid,
            password=payload.password,
            host=payload.host,
            port=payload.port,
            address=payload.address,
            ip=payload.ip,
            netmask=payload.netmask,
            gateway=payload.gateway,
            api_token=payload.api_token,
        )
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    try:
        upsert_wifi_profile(
            {
                "name": payload.ssid,
                "ssid": payload.ssid,
                "password": payload.password,
                "host": payload.host,
                "port": payload.port,
                "ip": payload.ip or "",
                "netmask": payload.netmask or "",
                "gateway": payload.gateway or "",
            }
        )
    except Exception as exc:  # noqa: BLE001
        print(f"[wifi-profile] auto-save skipped: {exc}")
    return result


@app.post("/api/device/wifi-profiles")
async def api_device_wifi_profiles():
    try:
        result = await ws_hub.request_wifi_profiles()
    except RuntimeError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    return {
        "ok": True,
        "via": "ws",
        "count": result.get("count"),
        "active": result.get("active"),
        "profiles": result.get("profiles") or [],
    }


@app.post("/api/device/wifi-profiles/save")
async def api_device_wifi_profile_save(payload: DeviceWifiProfileSave):
    body = payload.model_dump()
    body["type"] = "wifi_profile_save"
    if body.get("index") is None:
        body["index"] = -1
    try:
        result = await ws_hub.request_wifi_command(body)
    except RuntimeError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    return result


@app.post("/api/device/wifi-profiles/delete")
async def api_device_wifi_profile_delete(payload: DeviceWifiIndexEvent):
    try:
        result = await ws_hub.request_wifi_command(
            {"type": "wifi_profile_delete", "index": payload.index}
        )
    except RuntimeError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    return result


@app.post("/api/device/wifi-profiles/activate")
async def api_device_wifi_profile_activate(payload: DeviceWifiIndexEvent):
    try:
        result = await ws_hub.request_wifi_command(
            {"type": "wifi_profile_activate", "index": payload.index}
        )
    except RuntimeError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    return result


@app.post("/api/ble/read-profiles")
async def api_ble_read_profiles(payload: BleReadRequest = BleReadRequest()):
    try:
        result = await ble_prov.read_profiles(address=payload.address)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    imported = []
    for item in result.get("profiles") or []:
        try:
            imported.append(upsert_wifi_profile(item))
        except Exception as exc:  # noqa: BLE001
            print(f"[wifi-profile] import skipped: {exc}")
    device_profiles = [mask_wifi_profile(p) for p in (result.get("profiles") or [])]
    return {
        "ok": True,
        "address": result.get("address"),
        "count": result.get("count"),
        "active": result.get("active"),
        "device_profiles": device_profiles,
        "profiles": list_wifi_profiles(),
        "imported": [mask_wifi_profile(p) for p in imported],
        "replies": result.get("replies") or [],
        **ble_prov.status(),
    }

@app.get("/api/logs")
def api_logs():
    return {"items": log_hub.items()}


@app.get("/api/transcripts")
def api_transcripts():
    with state.lock:
        return {"items": list(state.transcripts)}


@app.websocket("/ws")
async def websocket_route(websocket: WebSocket):
    await ws_endpoint(ws_hub, websocket)


@app.get("/api/history")
def api_history():
    with state.lock:
        return {"items": list(state.history)}


@app.get("/health")
def health():
    return {"ok": True, "device": ws_hub.device_snapshot()}


@app.post("/event")
def event(payload: Event):
    try:
        normalized, sent = apply_event(payload)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    return {"ok": True, "queued": normalized, "debounced": not sent}


@app.post("/event/status")
def event_status(payload: StatusEvent):
    try:
        status_fields = payload.normalized()
        fp = _device_status_fingerprint(status_fields)
        with state.lock:
            if fp == state.last_device_status_fp:
                return {"ok": True, "queued": status_fields, "debounced": True}
            current = dict(state.current_event)
            current.update(status_fields)
            state.record_history(current)
            state.last_device_status_fp = fp
        if ws_hub.loop is not None:
            asyncio.run_coroutine_threadsafe(ws_hub.push_status_event(status_fields), ws_hub.loop)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    return {"ok": True, "queued": status_fields, "debounced": False}


@app.post("/event/text")
async def event_text(payload: TextEvent):
    try:
        text_fields = payload.normalized()
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    with state.lock:
        current = dict(state.current_event)
        current.update(text_fields)
        state.record_history(current)
    await ws_hub.push_text_event(text_fields)
    spoken = None
    if payload.speak:
        spoken = await ws_hub.speak_text(payload.text)
    return {"ok": True, "queued": text_fields, "spoken": spoken}


@app.post("/voice/upload")
async def voice_upload(request: Request):
    raw = await request.body()
    meta_header = request.headers.get("x-audio-meta", "")
    try:
        meta = json.loads(meta_header) if meta_header else {}
    except json.JSONDecodeError as exc:
        raise HTTPException(status_code=400, detail="invalid x-audio-meta") from exc
    if not raw:
        raise HTTPException(status_code=400, detail="missing pcm audio data")
    session = session_store.create(
        raw,
        sample_rate=int(meta.get("sample_rate", 16000)),
        channels=int(meta.get("channels", 1)),
        bit_depth=int(meta.get("bit_depth", 16)),
    )
    _push_voice_event("THINKING", "正在处理", "VOICE")
    start_pipeline(session.session_id, _push_voice_event, _on_transcript)
    return {"ok": True, "session_id": session.session_id}


@app.get("/voice/audio/{session_id}")
def voice_audio_chunk(session_id: str):
    session = session_store.get(session_id)
    if session is None:
        raise HTTPException(status_code=404, detail="session not found")
    chunk, is_end = session.pop_audio_chunk()
    if chunk is None:
        return Response(status_code=204, headers={"X-Audio-End": "1"} if is_end else {})
    headers = {"X-Audio-End": "1"} if is_end else {}
    return Response(content=chunk, media_type="application/octet-stream", headers=headers)

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
from fastapi.responses import HTMLResponse
from pydantic import BaseModel, Field

from agent_status import STATUS_DETAILS, TASK_LABELS, TOOL_CATEGORIES, normalize_status
from voice_pipeline import start_pipeline, warmup as voice_warmup
from voice_session import session_store
from ws_manager import WsHub, ws_endpoint

DASHBOARD_HTML = Path(__file__).with_name("dashboard.html")


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


class TextEvent(BaseModel):
    text: str = Field(default="", max_length=240)
    source: str = Field(default="BOT", max_length=16)

    def normalized(self) -> dict:
        source = self.source.upper()
        if source not in {"PI", "CURSOR", "UNKNOWN", "VOICE", "BOT"}:
            raise ValueError("unsupported source")
        return {"text": self.text, "source": source}


class AppState:
    def __init__(self):
        self.lock = threading.Lock()
        self.current_event = {"status": "IDLE", "source": "BOT", "text": ""}
        self.history = deque(maxlen=50)
        self.transcripts = deque(maxlen=200)

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


state = AppState()


def _push_voice_event(status: str, text: str, source: str = "VOICE") -> None:
    if text and len(text) > 240:
        text = text[:237] + "..."
    try:
        apply_event(Event(status=status, text=text, source=source))
    except ValueError as exc:
        print(f"[voice] push failed: {exc}")


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


def apply_event(event: Event) -> dict:
    normalized = event.normalized()
    with state.lock:
        state.record_history(normalized)
    if ws_hub.loop is not None:
        asyncio.run_coroutine_threadsafe(ws_hub.push_status_event(normalized), ws_hub.loop)
    return normalized


async def _sync_device_state(hub: WsHub) -> None:
    with state.lock:
        event = dict(state.current_event or {})
    await hub.push_status_event(event or {"status": "IDLE", "source": "BOT"})


ws_hub.set_on_device_connect(_sync_device_state)

HOOK_QUEUE = Path(os.getenv(
    "AGENT_DISPLAY_QUEUE",
    str(Path.home() / ".cursor" / "hooks" / "event_queue.jsonl"),
))
HOOK_ONLINE = HOOK_QUEUE.parent / "backend_online.json"
_hook_stop = threading.Event()
_hook_last_online = 0.0


def _touch_hook_online(force: bool = False) -> None:
    global _hook_last_online
    now = time.monotonic()
    if not force and now - _hook_last_online < 2.0:
        return
    try:
        HOOK_ONLINE.parent.mkdir(parents=True, exist_ok=True)
        HOOK_ONLINE.write_text(json.dumps({"ok": True, "ts": time.time()}), encoding="utf-8")
        _hook_last_online = now
    except OSError as exc:
        print(f"[hook] online file failed: {exc}")


def _drain_hook_queue() -> None:
    if not HOOK_QUEUE.exists() or HOOK_QUEUE.stat().st_size == 0:
        return
    try:
        raw = HOOK_QUEUE.read_text(encoding="utf-8")
        HOOK_QUEUE.write_text("", encoding="utf-8")
    except OSError as exc:
        print(f"[hook] queue read failed: {exc}")
        return
    for line in raw.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            data = json.loads(line)
            apply_event(Event(**data))
            print(f"[hook] applied {line}")
        except (json.JSONDecodeError, ValueError, TypeError) as exc:
            print(f"[hook] skipped: {exc} -> {line}")


def _hook_loop() -> None:
    HOOK_QUEUE.parent.mkdir(parents=True, exist_ok=True)
    _touch_hook_online(True)
    print(f"[hook] watching {HOOK_QUEUE}")
    while not _hook_stop.is_set():
        _touch_hook_online()
        _drain_hook_queue()
        _hook_stop.wait(0.1)




@asynccontextmanager
async def lifespan(_: FastAPI):
    ws_hub.bind_loop(asyncio.get_running_loop())
    pump_stop = threading.Event()
    pump_task = asyncio.create_task(ws_hub.run_session_pump(pump_stop))
    voice_warmup()
    _hook_stop.clear()
    hook_thread = threading.Thread(target=_hook_loop, daemon=True, name="hook-queue")
    hook_thread.start()
    yield
    _hook_stop.set()
    try:
        HOOK_ONLINE.unlink(missing_ok=True)
    except OSError:
        pass
    pump_stop.set()
    pump_task.cancel()
    try:
        await pump_task
    except asyncio.CancelledError:
        pass


app = FastAPI(title="Agent 状态显示后端", lifespan=lifespan)


@app.get("/", response_class=HTMLResponse)
def dashboard():
    return HTMLResponse(DASHBOARD_HTML.read_text(encoding="utf-8"))


@app.get("/api/status")
def api_status():
    with state.lock:
        current = dict(state.current_event)
        history = list(state.history)
        transcripts = list(state.transcripts)
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
        "history": history,
        "transcripts": transcripts,
        "updated": time.time(),
    }


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
        normalized = apply_event(payload)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    return {"ok": True, "queued": normalized, "debounced": False}


@app.post("/event/status")
def event_status(payload: StatusEvent):
    try:
        status_fields = payload.normalized()
        apply_event(Event(**{**status_fields, "text": ""}))
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    return {"ok": True, "queued": status_fields, "debounced": False}


@app.post("/event/text")
def event_text(payload: TextEvent):
    try:
        text_fields = payload.normalized()
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    with state.lock:
        current = dict(state.current_event)
        current.update(text_fields)
        state.record_history(current)
    if ws_hub.loop is not None:
        asyncio.run_coroutine_threadsafe(ws_hub.push_text_event(text_fields), ws_hub.loop)
    return {"ok": True, "queued": text_fields}


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

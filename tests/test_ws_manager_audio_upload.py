import asyncio

import ws_manager
from ws_manager import WsHub


class _WebSocket:
    def __init__(self):
        self.messages = []

    async def send_json(self, message):
        self.messages.append(message)


class _Session:
    session_id = "session-1"

    def mark_stream_asr(self, _text):
        pass


class _Recognizer:
    def __init__(self, *_args):
        pass

    def accept(self, _data):
        return ""

    def finish(self):
        return ""


def test_non_stream_upload_accepts_framed_binary(monkeypatch):
    hub = WsHub(lambda *_args: None)
    socket = _WebSocket()
    hub._device.websocket = socket
    hub._device.waiting_audio = {"audio_len": 4, "sample_rate": 16000, "channels": 1, "bit_depth": 16}

    captured = []
    monkeypatch.setattr(ws_manager.session_store, "create", lambda pcm, **_kwargs: captured.append(pcm) or _Session())
    monkeypatch.setattr(ws_manager, "VoskStreamRecognizer", _Recognizer)
    monkeypatch.setattr(ws_manager, "start_pipeline", lambda *_args: None)

    asyncio.run(hub.handle_binary(socket, b"ab"))
    assert hub._device.waiting_audio is not None
    assert captured == []

    asyncio.run(hub.handle_binary(socket, b"cd"))
    assert hub._device.waiting_audio is None
    assert captured == [b"abcd"]
    assert any(message.get("type") == "session" for message in socket.messages)

import json
from pathlib import Path

from hook_bridge import drain_hook_queue


def test_drain_hook_queue_atomic(tmp_path, monkeypatch):
    queue = tmp_path / "event_queue.jsonl"
    queue.write_text('{"status":"IDLE","text":"","source":"BOT"}\n', encoding="utf-8")
    monkeypatch.setattr("hook_bridge.HOOK_QUEUE", queue)

    applied = []
    drain_hook_queue(lambda data: applied.append(data))
    assert len(applied) == 1
    assert applied[0]["status"] == "IDLE"
    assert not queue.exists()
    assert not queue.with_suffix(".processing").exists()

    queue.write_text('{"status":"DONE","text":"","source":"BOT"}\n', encoding="utf-8")
    drain_hook_queue(lambda data: applied.append(data))
    assert len(applied) == 2

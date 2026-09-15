import json

from settings_store import clamp_volume_percent, get_volume_percent, set_volume_percent


def test_clamp_volume_percent():
    assert clamp_volume_percent(150) == 100
    assert clamp_volume_percent(-3) == 0
    assert clamp_volume_percent("42") == 42


def test_volume_roundtrip(tmp_path, monkeypatch):
    path = tmp_path / "settings.json"
    monkeypatch.setattr("settings_store.SETTINGS_PATH", path)
    monkeypatch.setattr("settings_store._volume_percent", None)
    monkeypatch.setattr("settings_store._voice_enabled", None)
    monkeypatch.setattr("settings_store._tts_voice", None)

    assert set_volume_percent(55) == 55
    assert get_volume_percent() == 55
    data = json.loads(path.read_text(encoding="utf-8"))
    assert data["volume_percent"] == 55

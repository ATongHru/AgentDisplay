from llm_history import append_record, query_records


def test_append_and_query(tmp_path, monkeypatch):
    history = tmp_path / "llm_chat.jsonl"
    monkeypatch.setattr("llm_history.HISTORY_FILE", history)
    monkeypatch.setattr("llm_history.LOG_DIR", tmp_path)

    append_record(user="hello", assistant="world", model="test", source="VOICE")
    result = query_records(page=1, page_size=10, q="hello")
    assert result["total"] == 1
    assert result["items"][0]["user"] == "hello"
    assert "path" not in result

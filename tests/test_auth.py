import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

import auth
from auth import AuthMiddleware, verify_token


@pytest.fixture()
def client(monkeypatch):
    monkeypatch.setattr(auth, "API_TOKEN", "secret")
    app = FastAPI()
    app.add_middleware(AuthMiddleware)

    @app.get("/api/status")
    def status():
        return {"ok": True}

    @app.get("/health")
    def health():
        return {"ok": True}

    return TestClient(app)


def test_verify_token(monkeypatch):
    monkeypatch.setattr(auth, "API_TOKEN", "secret")
    assert verify_token("secret") is True
    assert verify_token("wrong") is False
    assert verify_token(None) is False


def test_api_requires_token(client):
    assert client.get("/health").status_code == 200
    assert client.get("/api/status").status_code == 401
    assert client.get("/api/status", headers={"Authorization": "Bearer secret"}).status_code == 200

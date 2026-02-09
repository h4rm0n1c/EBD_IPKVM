"""Web client server shell for EBD IPKVM."""

from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Optional

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, JSONResponse

STATIC_DIR = Path(__file__).resolve().parent / "static"
INDEX_HTML = STATIC_DIR / "index.html"


@dataclass
class SessionState:
    active: bool = False
    owner_id: Optional[str] = None
    websocket: Optional[WebSocket] = None
    lock: asyncio.Lock = field(default_factory=asyncio.Lock)


class SessionManager:
    def __init__(self) -> None:
        self._state = SessionState()

    async def start(self, owner_id: str) -> Dict[str, Any]:
        async with self._state.lock:
            if self._state.active:
                return {
                    "active": True,
                    "owner_id": self._state.owner_id,
                    "message": "Session already active.",
                }
            self._state.active = True
            self._state.owner_id = owner_id
            return {"active": True, "owner_id": owner_id, "message": "Session started."}

    async def stop(self, owner_id: Optional[str]) -> Dict[str, Any]:
        async with self._state.lock:
            if not self._state.active:
                return {"active": False, "message": "Session already idle."}
            if owner_id and self._state.owner_id and owner_id != self._state.owner_id:
                raise HTTPException(status_code=403, detail="Session owned by another client.")
            self._state.active = False
            self._state.owner_id = None
            return {"active": False, "message": "Session stopped."}

    async def status(self) -> Dict[str, Any]:
        async with self._state.lock:
            return {"active": self._state.active, "owner_id": self._state.owner_id}

    async def attach_websocket(self, websocket: WebSocket) -> None:
        async with self._state.lock:
            if self._state.websocket is not None:
                await websocket.close(code=1008, reason="Another client is already connected.")
                raise HTTPException(status_code=409, detail="WebSocket already connected.")
            self._state.websocket = websocket

    async def detach_websocket(self, websocket: WebSocket) -> None:
        async with self._state.lock:
            if self._state.websocket is websocket:
                self._state.websocket = None


def create_app() -> FastAPI:
    app = FastAPI()
    session_manager = SessionManager()

    @app.get("/", response_class=HTMLResponse)
    async def index() -> HTMLResponse:
        if not INDEX_HTML.exists():
            raise HTTPException(status_code=500, detail="Missing UI template.")
        return HTMLResponse(INDEX_HTML.read_text(encoding="utf-8"))

    @app.post("/api/session/start")
    async def start_session(payload: Dict[str, Any]) -> JSONResponse:
        owner_id = str(payload.get("owner_id") or "browser-session")
        response = await session_manager.start(owner_id)
        return JSONResponse(response)

    @app.post("/api/session/stop")
    async def stop_session(payload: Dict[str, Any]) -> JSONResponse:
        owner_id = payload.get("owner_id")
        response = await session_manager.stop(owner_id)
        return JSONResponse(response)

    @app.get("/api/session/status")
    async def session_status() -> JSONResponse:
        return JSONResponse(await session_manager.status())

    @app.websocket("/ws")
    async def websocket_endpoint(websocket: WebSocket) -> None:
        await websocket.accept()
        try:
            await session_manager.attach_websocket(websocket)
            await websocket.send_json(
                {
                    "type": "status",
                    "message": "WebSocket connected. Session is idle by default.",
                }
            )
            while True:
                data = await websocket.receive_json()
                if data.get("type") == "console_input":
                    message = data.get("message", "")
                    await websocket.send_json(
                        {"type": "console_echo", "message": f"> {message}"}
                    )
                elif data.get("type") == "ping":
                    await websocket.send_json({"type": "pong"})
        except WebSocketDisconnect:
            await session_manager.detach_websocket(websocket)
        finally:
            await session_manager.detach_websocket(websocket)

    return app

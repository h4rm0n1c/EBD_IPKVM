"""Web client server shell for EBD IPKVM."""

from __future__ import annotations

import asyncio
import glob
import os
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Optional

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, JSONResponse

STATIC_DIR = Path(__file__).resolve().parent / "static"
INDEX_HTML = STATIC_DIR / "index.html"

W = 512
H = 342
LINE_BYTES = 64
HEADER_BYTES = 8
MAX_PAYLOAD = LINE_BYTES * 2
RLE_FLAG = 0x8000
LEN_MASK = 0x7FFF
MAGIC0 = 0xEB
MAGIC1 = 0xD1
STREAM_BAUD = 115200


@dataclass
class SessionState:
    active: bool = False
    owner_id: Optional[str] = None
    websocket: Optional[WebSocket] = None
    stream_task: Optional[asyncio.Task[None]] = None
    stream_stop: asyncio.Event = field(default_factory=asyncio.Event)
    lock: asyncio.Lock = field(default_factory=asyncio.Lock)


class SessionManager:
    def __init__(self) -> None:
        self._state = SessionState()

    async def _start_stream(self) -> None:
        if self._state.stream_task or self._state.websocket is None:
            return
        self._state.stream_stop.clear()
        self._state.stream_task = asyncio.create_task(
            stream_loop(self._state.websocket, self._state.stream_stop)
        )

    async def _stop_stream(self) -> None:
        task = self._state.stream_task
        if task is None:
            return
        self._state.stream_stop.set()
        await task
        self._state.stream_task = None

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
            await self._start_stream()
            return {"active": True, "owner_id": owner_id, "message": "Session started."}

    async def stop(self, owner_id: Optional[str]) -> Dict[str, Any]:
        async with self._state.lock:
            if not self._state.active:
                return {"active": False, "message": "Session already idle."}
            if owner_id and self._state.owner_id and owner_id != self._state.owner_id:
                raise HTTPException(status_code=403, detail="Session owned by another client.")
            self._state.active = False
            self._state.owner_id = None
            await self._stop_stream()
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
            if self._state.active:
                await self._start_stream()

    async def detach_websocket(self, websocket: WebSocket) -> None:
        async with self._state.lock:
            if self._state.websocket is websocket:
                self._state.websocket = None
                await self._stop_stream()


def auto_detect_stream_device() -> tuple[Optional[str], Optional[str]]:
    by_id_dir = "/dev/serial/by-id"
    if not os.path.isdir(by_id_dir):
        return None, f"{by_id_dir} is missing; CDC auto-detect is unavailable."
    entries = sorted(glob.glob(os.path.join(by_id_dir, "*")))
    matches = [e for e in entries if "if00" in os.path.basename(e)]
    if not matches:
        return None, "CDC stream port not found (expected /dev/serial/by-id/*if00*)."
    if len(matches) > 1:
        joined = "\n  ".join(matches)
        return None, f"Multiple CDC stream ports found:\n  {joined}"
    return matches[0], None


def pop_one_packet(buf: bytearray) -> Optional[bytes]:
    n = len(buf)
    i = 0
    while i + 1 < n and not (buf[i] == MAGIC0 and buf[i + 1] == MAGIC1):
        i += 1
    if i > 0:
        del buf[:i]
    if len(buf) < HEADER_BYTES:
        return None
    plen = buf[6] | (buf[7] << 8)
    payload_len = plen & LEN_MASK
    if payload_len == 0 or payload_len > MAX_PAYLOAD:
        del buf[:2]
        return None
    total_len = HEADER_BYTES + payload_len
    if len(buf) < total_len:
        return None
    pkt = bytes(buf[:total_len])
    del buf[:total_len]
    return pkt


async def stream_loop(websocket: WebSocket, stop_event: asyncio.Event) -> None:
    import serial

    stream_device = os.environ.get("EBD_IPKVM_STREAM_DEVICE")
    if not stream_device:
        stream_device, err = auto_detect_stream_device()
        if err:
            await websocket.send_json({"type": "error", "message": err})
            return
    await websocket.send_json(
        {"type": "status", "message": f"Opening CDC stream device: {stream_device}"}
    )
    try:
        stream = serial.Serial(stream_device, baudrate=STREAM_BAUD, timeout=0.5)
    except serial.SerialException as exc:
        await websocket.send_json(
            {"type": "error", "message": f"Failed to open stream device: {exc}"}
        )
        return
    stream.dtr = True
    stream.rts = True
    buf = bytearray()
    try:
        while not stop_event.is_set():
            chunk = await asyncio.to_thread(stream.read, 8192)
            if not chunk:
                continue
            buf.extend(chunk)
            while True:
                pkt = pop_one_packet(buf)
                if pkt is None:
                    break
                frame_id = pkt[2] | (pkt[3] << 8)
                line_id = pkt[4] | (pkt[5] << 8)
                plen = pkt[6] | (pkt[7] << 8)
                payload_len = plen & LEN_MASK
                if payload_len == 0 or payload_len > MAX_PAYLOAD or line_id >= H:
                    continue
                payload = pkt[8 : 8 + payload_len]
                header = struct.pack("<HHHH", frame_id, line_id, plen, 0)
                await websocket.send_bytes(header + payload)
    finally:
        stream.close()


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

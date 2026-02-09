"""Web client server shell for EBD IPKVM."""

from __future__ import annotations

import asyncio
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
USB_VID = 0x2E8A
USB_PID = 0x000A
CTRL_REQ_CAPTURE_START = 0x01
CTRL_REQ_CAPTURE_STOP = 0x02
CTRL_REQ_RESET_COUNTERS = 0x03
CTRL_REQ_RLE_ON = 0x05

DEFAULT_BOOT_WAIT_S = 12.0
DEFAULT_DIAG_SECS = 12.0
DEFAULT_CTRL_MODE = "ep0"


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
            if self._state.websocket is not None:
                await run_control_sequence(self._state.websocket)
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
            if self._state.websocket is not None:
                try:
                    dev = await asyncio.to_thread(open_usb_device_for_control)
                    await asyncio.to_thread(send_ep0_cmd, dev, CTRL_REQ_CAPTURE_STOP)
                    await self._state.websocket.send_json(
                        {"type": "status", "message": "EP0: capture stop"}
                    )
                except RuntimeError as exc:
                    await self._state.websocket.send_json(
                        {"type": "error", "message": str(exc)}
                    )
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


def open_usb_stream() -> tuple[Any, int, Any]:
    try:
        import usb.core
        import usb.util
    except ImportError as exc:
        raise RuntimeError(
            f"pyusb not available: {exc}. Install python3-usb or pyusb."
        ) from exc

    dev = usb.core.find(idVendor=USB_VID, idProduct=USB_PID)
    if dev is None:
        raise RuntimeError("USB device not found (VID/PID 0x2E8A:0x000A).")

    try:
        cfg = dev.get_active_configuration()
    except usb.core.USBError:
        cfg = None
    if cfg is None:
        try:
            dev.set_configuration()
        except usb.core.USBError as exc:
            if getattr(exc, "errno", None) != 16:
                raise
        cfg = dev.get_active_configuration()
    intf = usb.util.find_descriptor(cfg, bInterfaceClass=0xFF)
    if intf is None:
        raise RuntimeError("Bulk stream interface not found.")

    if dev.is_kernel_driver_active(intf.bInterfaceNumber):
        dev.detach_kernel_driver(intf.bInterfaceNumber)

    usb.util.claim_interface(dev, intf.bInterfaceNumber)
    ep_in = usb.util.find_descriptor(
        intf,
        custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress)
        == usb.util.ENDPOINT_IN,
    )
    if ep_in is None:
        raise RuntimeError("Bulk IN endpoint not found.")
    return dev, intf.bInterfaceNumber, ep_in


def read_usb_stream(ep_in: Any, timeout_s: float) -> bytes:
    timeout_ms = int(timeout_s * 1000)
    try:
        data = ep_in.read(8192, timeout=timeout_ms)
        return bytes(data)
    except Exception:
        return b""


def open_usb_device_for_control() -> Any:
    try:
        import usb.core
    except ImportError as exc:
        raise RuntimeError(
            f"pyusb not available: {exc}. Install python3-usb or pyusb."
        ) from exc

    dev = usb.core.find(idVendor=USB_VID, idProduct=USB_PID)
    if dev is None:
        raise RuntimeError("USB device not found (VID/PID 0x2E8A:0x000A).")

    try:
        dev.get_active_configuration()
    except Exception:
        try:
            dev.set_configuration()
        except Exception:
            pass
    return dev


def send_ep0_cmd(dev: Any, req: int) -> None:
    try:
        dev.ctrl_transfer(0x41, req, 0, 0, None)
    except Exception as exc:
        raise RuntimeError(f"EP0 control transfer failed (req=0x{req:02X}): {exc}") from exc


async def run_control_sequence(
    websocket: WebSocket,
    *,
    boot_wait_s: float = DEFAULT_BOOT_WAIT_S,
    diag_secs: float = DEFAULT_DIAG_SECS,
) -> None:
    if DEFAULT_CTRL_MODE != "ep0":
        return
    try:
        dev = await asyncio.to_thread(open_usb_device_for_control)
    except RuntimeError as exc:
        await websocket.send_json({"type": "error", "message": str(exc)})
        return

    if diag_secs > 0:
        await websocket.send_json(
            {"type": "status", "message": f"diag: waiting {diag_secs:.2f}s before start"}
        )
        await asyncio.sleep(diag_secs)

    if boot_wait_s > diag_secs:
        await asyncio.sleep(boot_wait_s - diag_secs)

    for req, note in (
        (CTRL_REQ_CAPTURE_STOP, "capture stop"),
        (CTRL_REQ_RESET_COUNTERS, "reset counters"),
        (CTRL_REQ_RLE_ON, "enable RLE"),
        (CTRL_REQ_CAPTURE_START, "capture start"),
    ):
        try:
            await asyncio.to_thread(send_ep0_cmd, dev, req)
        except RuntimeError as exc:
            await websocket.send_json({"type": "error", "message": str(exc)})
            return
        await websocket.send_json({"type": "status", "message": f"EP0: {note}"})


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
    try:
        dev, intf_num, ep_in = open_usb_stream()
    except RuntimeError as exc:
        await websocket.send_json(
            {"type": "error", "message": f"Failed to open USB stream: {exc}"}
        )
        return
    buf = bytearray()
    try:
        while not stop_event.is_set():
            chunk = await asyncio.to_thread(read_usb_stream, ep_in, 0.25)
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
        try:
            import usb.util

            usb.util.release_interface(dev, intf_num)
        except Exception:
            pass


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

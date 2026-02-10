# EBD IPKVM Web Client

## Overview
This folder contains the planned web client for EBD IPKVM. The web service is **idle by default** and only connects to devices after the user explicitly starts a session from the web UI.
This is a **single-session, single-client** UI: one browser session owns one set of connections to the Pico (and later the MacFriends Arduino), and multi-user concurrency is explicitly out of scope.

## Goals
- Provide a browser-based control and monitoring UI.
- Keep the service idle until the UI triggers a session.
- Maintain parity with the existing host tooling over time.

## Run (placeholder)
`pipx run ebd-ipkvm-web --host 0.0.0.0 --port 8000`

## Local development (prototype)
This initial prototype uses FastAPI + Uvicorn for the UI shell and a placeholder session API.
Run it directly with:

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -e . --upgrade
python -m ebd_ipkvm_web
```

The server binds to `0.0.0.0:8000` by default so you can access it from other machines on your LAN.
If you still cannot reach it remotely, check your firewall or run `uvicorn` explicitly with `--host 0.0.0.0`.
Only one WebSocket client can be connected at a time; close any existing browser session before connecting from another machine.
`crypto.randomUUID` is only available in secure contexts; remote HTTP access can lack it, so the UI uses a fallback ID generator.

The dependencies include `uvicorn[standard]` so WebSocket support is available for the CDC0 console panel.
The video stream uses the USB bulk interface (pyusb + EP0 control), matching `host_recv_frames.py`.

### Troubleshooting missing dependencies
If you see `ModuleNotFoundError: No module named 'serial'`, the fix is to upgrade the editable install so `pyserial` is pulled in:

```sh
. .venv/bin/activate
pip install -e . --upgrade
```

If you see `pyusb not available`, install the dependency and refresh the venv:

```sh
. .venv/bin/activate
pip install -e . --upgrade
```

On Debian/Devuan you may also need the system package:

```sh
sudo apt-get install -y python3-usb
```

## Devuan setup (PEP 668 + Serial Permissions)
### PEP 668 (Externally Managed Environments)
Devuan enables PEP 668 by default, so `pip install` into the system Python is blocked. Use `pipx` for isolated installs.

### Packages
Install `pipx`, `python3-venv`, and any system dependencies needed by the web client:

```sh
sudo apt-get update
sudo apt-get install -y pipx python3-venv
```

Ensure `pipx` is on your PATH:

```sh
pipx ensurepath
```

### Serial Permissions
USB CDC devices often appear as `/dev/ttyACM*` or `/dev/ttyUSB*`. Add your user to `dialout` so the web client can open the device when you start a session from the UI:

```sh
sudo usermod -aG dialout "$USER"
```

Log out and back in to apply the group change.

## Documentation
- Setup/usage notes live in this README as the client grows.


## ADB mouse bridge (web canvas → Arduino serial)
The web client now captures pointer-locked mouse input on the video canvas and forwards relative deltas + left-button state to the MacFriends Arduino core over serial.

- Default serial discovery glob: `/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_*-if00-port0`
- Override with `ADB_SERIAL_PORT` (exact path or glob)
- Packet format matches the MacFriends host client (`magic=123`, `updateType=1`, signed `dx`/`dy`)

Example:

```sh
export ADB_SERIAL_PORT='/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_*-if00-port0'
python -m ebd_ipkvm_web
```

In the UI, click the video canvas to lock the pointer. Movement, left-click, and keyboard keydown/keyup events are sent to the Arduino while the capture session is active. Mouse movement is currently scaled to 0.35x in the browser to compensate for the 2x canvas display, and each outbound packet is capped to ±12 counts per axis to smooth fast pointer bursts. Right-click exits pointer lock so Escape can be passed through to the Mac.

If you enable **Boot for ROM disk** before starting a session, the web backend asserts the ROM-disk chord (`Command+Option+X+O`) before power-on, then reasserts it periodically through the first ~10 seconds.

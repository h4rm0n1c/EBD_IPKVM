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

The dependencies include `uvicorn[standard]` so WebSocket support is available for the CDC console panel.
Set `EBD_IPKVM_STREAM_DEVICE` to override CDC stream auto-detect (expects `/dev/serial/by-id/*if00*`).

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

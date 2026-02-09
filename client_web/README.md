# EBD IPKVM Web Client

## Overview
This folder contains the planned web client for EBD IPKVM. The web service is **idle by default** and only connects to devices after the user explicitly starts a session from the web UI.

## Goals
- Provide a browser-based control and monitoring UI.
- Keep the service idle until the UI triggers a session.
- Maintain parity with the existing host tooling over time.

## Run (placeholder)
`pipx run ebd-ipkvm-web --host 0.0.0.0 --port 8000`

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
- Setup/usage notes: add files under `docs/` as the client grows.

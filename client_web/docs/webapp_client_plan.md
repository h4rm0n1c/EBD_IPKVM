# Webapp Client Plan (Ordered, Parity-First)

We should not do this all in one block all at once. This is the staged plan for delivery over time, and we will keep progress updated here as tasks are completed.

## Implementation references (keep in sync as we learn more)
- `src/host_recv_frames.py` is the primary reference for CDC stream decode, frame assembly, and Pico CDC comms expectations; use it for parity checks when porting logic into the web client.

## Step 1 — Create the client app directory + baseline docs
:::task-stub{title="Add a dedicated web client directory and baseline README"}
Create `client_web/` with:
- `README.md` (overview, goals, one-line run command placeholder).
- `docs/` for setup and usage notes.
- `src/` for application code.
Document that the service starts idle and only connects to devices when triggered from the web UI.
:::

## Step 2 — Document Devuan + pipx setup (PEP 668 friendly)
:::task-stub{title="Document Devuan setup with pipx and serial permissions"}
In `client_web/README.md` or `client_web/docs/devuan.md`:
- Explain PEP 668 constraints and recommend `pipx`.
- Include package install steps for `pipx`, `python3-venv`, and any system deps.
- Add a note about `dialout`/udev permissions for `/dev/ttyACM*` and `/dev/ttyUSB*`.
:::

## Step 3 — Define the one-line run command and idle-by-default behavior
:::task-stub{title="Add one-line run command and idle-by-default service behavior"}
In the README:
- Provide a one-line run command (e.g., `pipx run ebd-ipkvm-web --host 0.0.0.0 --port 8000`).
- Explicitly state that the service starts idle and only connects to devices when the web UI triggers a session.
- Explicitly call out that the web client is single-session/single-client with one set of device connections.
Add config flags for default devices but don’t auto-connect at startup.
:::

## Step 4 — Implement the web server shell + UI skeleton
:::task-stub{title="Create the web UI shell with Start/Stop and CDC1 live console"}
Implement a basic web server with:
- `/` serving a simple HTML UI.
- WebSocket endpoint for frame updates and CDC1 console streaming.
UI components:
- “Start Capture / Boot Mac” button.
- “Stop” button.
- Video canvas area.
- CDC1 live console (scrollback + single-line input, send on Enter).
The UI must be present before any device connections are made.
:::

## Step 5 — Achieve parity with host_recv_frames (video pipeline)
:::task-stub{title="Port host_recv_frames decode logic into the webapp"}
Extract and reuse the core CDC stream decode logic from `src/host_recv_frames.py`:
- CDC stream device discovery/override.
- Line decoding + RLE handling.
- Frame buffer assembly.
Convert frames to a browser-friendly format (e.g., PNG) and stream over WebSocket to the UI’s canvas.
Maintain parity with current options (raw/pgm/pbm handling where relevant to the pipeline).
:::

## Step 6 — Add CDC1 live console passthrough
:::task-stub{title="Implement a live CDC1 console passthrough"}
Add a persistent CDC1 read loop that streams output to the browser console.
The console must:
- Show live scrollback.
- Send on Enter (no separate send button).
- Provide optional clear/reset.
Start CDC1 only after “Start Capture / Boot Mac” and tear down on “Stop.”
:::

## Step 7 — Session control flow (Start/Stop / Boot Mac)
:::task-stub{title="Add a Boot Mac session flow that starts capture and console"}
Implement “Start Capture / Boot Mac” that:
- Opens CDC stream.
- Starts video decode/streaming.
- Starts CDC1 live console.
Add “Stop” to close devices and return to idle state.
:::

## Step 8 — Future network transport notes
:::task-stub{title="Document transport abstraction for future network mode"}
Add a short doc (client README or `docs/architecture.md`) describing:
- A transport interface with USB CDC now, network sockets later.
- Where the transport boundary sits in the code.
- The intent to keep UI unchanged while transport swaps.
:::

---

# MacFriends (Lower Priority / After Parity)

## Step 9 — Add MacFriends USB-serial testing support
:::task-stub{title="Integrate MacFriends USB-serial testing into the web client"}
Add a MacFriends serial backend (pyserial) to send 8-byte ADBInstruction packets:
- Magic=123, updateType, mouseIsDown, dx, dy, keyCode, isKeyUp, modifierKeys.
Expose UI controls for mouse delta, button state, and keyboard events.
Open the MacFriends serial device only after the user clicks “Start.”

Also document the expected FTDI by-id path for the MacFriends dongle so it can be discovered generically:
`/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_*-if00-port0`
:::

## Step 10 — Input capture inside video area for MacFriends
:::task-stub{title="Capture input only over the video area and relay to MacFriends"}
In the UI:
- Listen to mouse/keyboard events only when the cursor is over the video canvas.
- Convert events to the MacFriends packet format and send them to the backend.
Add a toggle to enable/disable input capture if needed.
:::

# ADB peripheral implementation plan (keyboard + mouse)

## Goals

- Emulate ADB keyboard + mouse as on-bus peripherals for IP KVM use.
- Follow the existing core split pattern: PIO does bit-level timing, AppleCore (core1) handles real-time bus work, core0 exposes a higher-level API for the app and CDC testing.
- Base our low-level ADB engine on the hootswitch RP2040 implementation (PIO + DMA + state machine) and trim it down to a single-device keyboard+mouse emulation.
- Keep the ADB implementation modular and consistent with existing firmware structure (peripheral-specific files + shared core bridge).
- Add a CDC test channel so we can drive mouse movement via arrow keys, click via Ctrl+R, and pass through typed characters to the ADB keyboard.

## Reference grounding (from /opt/adb)

- **ADB Manager PDF:** ADB devices must implement default address + handler IDs, respond to Talk/Listen transactions, detect collisions, and assert SRQ when they have data to report. Devices expose up to four registers used by the bus manager and host driver stack.
- **AN591B (Microchip ADB note):** Detailed timing and bus waveform reference; use to verify pulse widths, SRQ windows, and stop-bit behavior.
- **hootswitch (RP2040 ADB switch):** Provides a working PIO + DMA bus engine. Its `bus.pio` supplies host/device TX/RX/attention programs, while `computer.c` shows device-side state tracking, command parsing, collision handling, and SRQ gating.
- **trabular (ADB device firmware):** Implements keyboard + mouse emulation, shows default addresses (keyboard=2, mouse=3), handler IDs, SRQ accounting, and timing windows used to interpret ADB bit cells.
- **adb-usb (ADB host firmware):** Provides a reference for bit-cell timing and signal shape assumptions in a simple ADB implementation.

These sources anchor our bus timing expectations, device register behaviors, and address handling.

- Cross-check behavior against the reference implementations in `/opt/adb` as we validate timing, register contents, and SRQ behavior.

## Architecture overview

### Hardware wiring (current board)

- **ADB RECV:** GPIO6, non-inverting input from the bus (buffered).
- **ADB XMIT:** GPIO12, inverted open-collector output (GPIO high pulls the bus low).
- PIO output polarity must be inverted to account for the transmit inversion.

### Hootswitch extraction scope

- Hootswitch uses two roles: **host** (front peripherals) on PIO1 and **computer/device** (emulated devices per computer port) on PIO0. For EBD_IPKVM we only need the **device-side** pipeline, so we can drop the host-side command queue, device scanning, and port switching logic.
- The device-side code (`computer.c`) already implements a bus phase state machine (attention → command → listen/talk) plus collision detection and SRQ gating. We can adapt this for a single virtual keyboard and mouse while keeping the timing logic intact.
- The PIO program swapping used on the host side can be omitted; we will keep a fixed set of device programs in PIO1 and use a single SM for our bus, with DMA for Listen data where needed.

### 1) PIO layer (bit timing + open-collector control)

- **Purpose:** Provide deterministic ADB line sampling and drive timing using dedicated PIO programs for RX/TX, based directly on hootswitch `bus.pio`.
- **Signal model:** ADB is open-collector. We should drive the line low via GPIO output enable and release to float high (external pull-up).
- **PIO placement:** Use **PIO1** for ADB to avoid contention with PIO0, which already hosts classic video capture.
- **PIO program split (from hootswitch):**
  - `bus_tx_dev`: device-side transmit with collision detection (Talk responses).
  - `bus_rx_dev`: device-side receive for commands/listens with SRQ gating at stop bit.
  - `bus_atn_dev`: attention/reset detector for low-pulse qualification.
  - `bus_reset`: optional reset pulse generator (not required for device emulation but useful for test tools).
- **PIO responsibilities:**
  - Detect attention, sync, and start/stop bits.
  - Read/serialize 8-bit data payloads and stop bits into RX FIFO.
  - Emit per-bit pulse sequences for Talk responses and SRQ assertion.
  - Preserve hootswitch’s 125 MHz timing assumptions unless we explicitly retune the PIO clock dividers.

### 2) AppleCore (core1): ADB real-time service loop

- **Purpose:** Interpret PIO edge/data events, handle exact bus timing, and manage device register access.
- **Scheduling target:** ADB handling should run every ~50–70 µs to avoid missing line transitions (per trabular timing notes) and respect bit timing windows.
- **Responsibilities:**
  - Parse command bytes (Talk/Listen/Flush/Reset) from PIO RX FIFO.
  - Detect address collisions and follow address resolution rules.
  - Ignore our own transmissions on the shared ADB line during normal operation (RX/TX pins are tied together on-bus); optionally gate a loopback/echo path when validating TX timing in isolation.
  - Latch RX activity so the CDC test channel can emit a rate-limited “ADB RX seen” line (e.g., every few seconds) when we decode valid bus traffic from the host.
  - Assert SRQ when keyboard/mouse buffers have new data.
  - Serialize Talk responses from per-device register state.
  - Apply Listen writes to device registers.
  - Use DMA for Listen data transfers (mirroring hootswitch) to reduce ISR load and stay within the video capture budget.

### 3) Core0: ADB device interface + CDC testing

- **Purpose:** Provide the high-level API used by the main application and the CDC test channel.
- **Responsibilities:**
  - Maintain keyboard/mouse event queues (key press/release sequences, mouse deltas, button state).
  - Translate higher-level input events into register payloads (keyboard register 0/2, mouse register 0).
  - Manage testing input on the third CDC interface:
    - Arrow keys -> mouse move (configurable step size).
    - Ctrl+R -> mouse button click.
    - Other printable characters -> ADB keyboard events.
    - Periodic RX indicator: if valid ADB traffic was observed, emit a short “ADB RX seen” line no more than once every few seconds (latched/ratelimited).
  - Forward events to core1 via `core_bridge` queues.

### 4) Core bridge between cores

- Extend the existing core bridge with an **ADB control channel**:
  - `core0 -> core1`: enqueue ADB events (kbd keycode, key up/down, mouse delta, button state).
  - `core1 -> core0`: optional diagnostics (SRQ count, collision stats, reset count, last command).

## Data model & register handling

### Keyboard registers

- **Default address:** 2 (from trabular).
- **Handler ID:** 2 (adb Extended Keyboard II is a common default in trabular; validate final ID against ADB spec references as needed).
- **Register 0:** key event pairs (two keycodes per report, with 0xFF for empty slots).
- **Register 2:** modifier bits + LED flags. Track LED state for Host Listen writes.
- **SRQ:** asserted whenever a new key event is queued (until Talk drains it).

### Mouse registers

- **Default address:** 3 (from trabular).
- **Handler ID:** use the initialized handler ID from the ADB spec (trabular keeps an init handler, so follow that pattern).
- **Register 0:** button + X/Y deltas (signed 7-bit or 8-bit based on device flavor; validate before final implementation).
- **SRQ:** asserted whenever a new mouse report is available.

### Address resolution + collision detection

- Use the ADB Manager’s address resolution flow: if multiple devices share an address, respond per the bus collision rules and allow host reassign via Listen.
- Track collision flags per device like trabular does, clearing once a new address is assigned.

## CDC test interface (third CDC channel)

- Add **CDC2** in `usb_descriptors.c` and update TUSB configuration.
- In `app_core`, add a small terminal parser:
  - Arrow keys (escape sequences): update mouse X/Y deltas.
  - Ctrl+R: click press/release toggle (press on keydown, release on keyup).
  - Other characters: map to ADB keyboard keycodes and enqueue a press/release.
- Keep this in a separate `adb_test_cdc.c/.h` module for cleanliness.

## File/module breakdown

**New modules (proposed):**

- `src/adb_pio.pio` / `src/adb_pio.c/.h`
  - PIO program definition, loader, and helper APIs derived from hootswitch `bus.pio`.
- `src/adb_bus.c/.h`
  - Core1-facing ADB bus logic (command parse, timing state, SRQ control).
- `src/adb_kbd.c/.h`
  - Keyboard register model + event queue.
- `src/adb_mouse.c/.h`
  - Mouse register model + event queue.
- `src/adb_core.c/.h`
  - Core1 dispatch loop wrapper that integrates with AppleCore and PIO ISR.
- `src/adb_test_cdc.c/.h`
  - CDC test channel parser and ADB event injection for development.

**Existing modules to update:**

- `src/usb_descriptors.c` + `src/tusb_config.h`: third CDC interface.
- `src/app_core.c`: add CDC2 handling and high-level ADB queue submission.
- `src/core_bridge.c/.h`: new ADB event queue.
- `src/main.c`: initialize ADB core components.
- Documentation updates: `README.md` + `docs/protocol/` (if new CDC channel or command specs are added).

## Coexistence with video capture

- Use PIO1 for ADB to avoid interfering with PIO0 video capture.
- Keep ADB service time in AppleCore (core1) to a tight budget and avoid blocking operations.
- Use lightweight queues to minimize lock contention and avoid interfering with the existing frame TX queue.
- Instrument core1 utilization around ADB work so we can confirm we stay well within the remaining budget (AppleCore reports ~12% usage currently).

## Implementation steps

1. **PIO prototype**: draft `adb_pio.pio` for line sampling + bit emission; validate timing in isolation.
2. **Core1 ADB engine**: implement bus state machine (`adb_bus`) with command parsing and SRQ emission.
3. **Keyboard/mouse models**: implement register logic and event queues, including default addresses/handler IDs and Listen/Flush behaviors.
4. **Core bridge**: add ADB event queues and event injection APIs on core0.
5. **CDC test channel**: add CDC2 descriptors, parse arrow keys and Ctrl+R, map to ADB events.
6. **Integrate with main**: init ADB modules alongside AppleCore; ensure idle states do not affect capture.
7. **Instrumentation + docs**: add optional diagnostics for SRQ/traffic and update docs (protocol + ADB plan/log/notes).

## Test plan

- **Basic bus health**: scope the ADB line for attention/sync, Talk responses, and SRQ pulses.
- **Terminal-driven test**: open CDC2 from Linux and verify:
  - Arrow keys move the Mac cursor.
  - Ctrl+R clicks.
  - Typed characters appear on the Mac.
- **Collision/address resolution**: connect a real ADB keyboard or mouse and ensure our device reassigns or coexists properly.
- **Performance**: check core0/core1 utilization counters while exercising ADB input + full-rate video capture.

## Open questions / follow-ups

- Confirm exact mouse report format (classic vs extended mouse) before final register definitions.
- Confirm keyboard handler ID and LED behavior match expected Mac OS defaults.
- Validate SRQ timing windows against the ADB spec once the Apple spec PDF is available.

## Hootswitch port status (code landed)

### Implemented from hootswitch

- Device-side PIO programs (`bus_rx_dev`, `bus_tx_dev`, `bus_atn_dev`) wired on PIO1 with the hootswitch timing assumptions and collision IRQ flagging.
- Device-side bus state machine: attention detect → command parse → SRQ phase → Talk/Listen handling (GPIO rise gating) matching hootswitch flow.
- SRQ timing handled in the PIO RX program (stop-bit SRQ pulse), with SRQ gating driven by a shared bitfield.
- Talk register locking and reg0 queue-drain semantics (only fill reg0 when empty + lock available).
- Reg3 randomized nibble + handler-ID gating (0xFF suppresses reg3 Talk).
- Listen semantics mirroring hootswitch (short writes clear reg0/SRQ; reg3 short writes ignored).
- Collision and lock-failure counters surfaced via CDC debug output.

### Not yet validated / still needed

- Hardware validation of ADB timing windows vs the ADB Manager spec (scope/SRQ pulse width, bit timing).
- Confirm handler IDs and keyboard LED behavior against Mac OS expectations.
- Confirm mouse report format (classic vs extended) and adjust if needed.
- Add documented host-driven validation steps once hardware captures are available.

## Hootswitch parity checklist

Use this as a concise checklist for what we must match from hootswitch during extraction.
Device-side parity is the goal; host/port-switching logic stays out of scope.

### Required parity items (device-side)

- Device-side PIO programs: `bus_rx_dev`, `bus_tx_dev`, `bus_atn_dev` (timing aligned to hootswitch defaults unless we explicitly retune).
- Stop-bit/SRQ phase gating before executing Talk/Listen (GPIO rise flow).
- SRQ timing handled by PIO (device-side SRQ pulse flow).
- SRQ pending tracking via the shared hootswitch-style bitfield.
- Reg3 randomized nibble + handler-ID gating (`0xFF` suppresses reg3 Talk).
- Talk register locking + reg0 queue-drain semantics (only fill when empty/unlocked).
- Listen-length semantics: short writes clear reg0/SRQ; short reg3 writes ignored.
- Collision/lock debug counters exposed for parity diagnostics.

### Explicitly out of scope

- Host-side PIO programs, device scanning, and port-switching logic from hootswitch.

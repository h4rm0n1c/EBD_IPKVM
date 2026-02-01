# ADB peripheral implementation plan (keyboard + mouse)

## Goals

- Emulate ADB keyboard + mouse as on-bus peripherals for IP KVM use.
- Follow the existing core split pattern: PIO does bit-level timing, core1 handles real-time bus work, core0 exposes a higher-level API for the app and CDC testing.
- Keep the ADB implementation modular and consistent with existing firmware structure (peripheral-specific files + shared core bridge).
- Add a CDC test channel so we can drive mouse movement via arrow keys, toggle the mouse button (currently `!`), and pass through typed characters to the ADB keyboard.

## Implementation status (active)

- ADB device emulation is **actively being implemented** (not deferred). The first goal is a minimal keyboard + mouse device that can pass a basic Mac desktop input test (move cursor, click, type characters) over the shared ADB bus.
- Use the steps in **First bring-up attempt** to get to the initial hardware/firmware validation run.
- Current firmware can decode command bytes and emit a minimal Talk response for keyboard register 0 (address 2) using queued CDC2 key events; Listen/SRQ handling is still pending.

## Reference grounding (from /opt/adb)

- **ADB Manager PDF:** ADB devices must implement default address + handler IDs, respond to Talk/Listen transactions, detect collisions, and assert SRQ when they have data to report. Devices expose up to four registers used by the bus manager and host driver stack.
- **trabular (ADB device firmware):** Implements keyboard + mouse emulation, shows default addresses (keyboard=2, mouse=3), handler IDs, SRQ accounting, and timing windows used to interpret ADB bit cells.
- **adb-usb (ADB host firmware):** Provides a reference for bit-cell timing and signal shape assumptions in a simple ADB implementation.

These sources anchor our bus timing expectations, device register behaviors, and address handling.

- Cross-check behavior against the reference implementations in `/opt/adb` as we validate timing, register contents, and SRQ behavior.

## Architecture overview

### 1) PIO layer (bit timing + open-collector control)

- **Purpose:** Provide deterministic ADB line sampling and drive timing using dedicated PIO programs for RX and TX.
- **Status:** Initial `adb_pio` RX/TX programs are now in-tree to capture low-pulse widths and drive low pulses; full bit-cell timing and command framing still need to be implemented.
- **Signal model:** ADB is open-collector. We should drive the line low via GPIO output enable and release to float high (external pull-up).
- **PIO placement:** Use **PIO1** for ADB to avoid contention with PIO0, which already hosts classic video capture.
- **PIO program split:** Plan for two PIO programs (or state machines) on PIO1: one tuned for RX capture and one for TX bit-cell generation. This mirrors the bidirectional nature of ADB and keeps timing logic simple per direction.
- **PIO responsibilities:**
  - Detect attention, sync, and start/stop bits.
  - Read/serialize 8-bit data payloads and stop bits into RX FIFO.
  - Emit per-bit pulse sequences for Talk responses and SRQ assertion.

### 2) Core1 (KVMCore): ADB real-time service loop

- **Purpose:** Interpret PIO edge/data events, handle exact bus timing, and manage device register access.
- **Scheduling target:** ADB handling should run every ~50–70 µs to avoid missing line transitions (per trabular timing notes) and respect bit timing windows.
- **Responsibilities:**
  - Parse command bytes (Talk/Listen/Flush/Reset) from PIO RX FIFO.
  - Detect address collisions and follow address resolution rules.
  - Ignore our own transmissions on the shared ADB line during normal operation (RX/TX pins are tied together on-bus); optionally gate a loopback/echo path when validating TX timing in isolation.
  - Latch RX activity so the CDC test channel can emit a rate-limited “ADB RX seen” line (e.g., every few seconds) when we decode valid bus traffic from the host.
  - Assert SRQ when keyboard/mouse buffers have new data.
  - Track queued ADB events waiting to be transmitted so we can verify key holds are pending before the TX path is implemented.
  - Serialize Talk responses from per-device register state.
  - Apply Listen writes to device registers.

### 3) Core0: ADB device interface + CDC testing

- **Purpose:** Provide the high-level API used by the main application and the CDC test channel.
- **Responsibilities:**
  - Maintain keyboard/mouse event queues (key press/release sequences, mouse deltas, button state).
  - Translate higher-level input events into register payloads (keyboard register 0/2, mouse register 0).
  - Manage testing input on the third CDC interface:
    - Arrow keys -> mouse move (configurable step size).
    - `!` toggles the mouse button state.
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
- Assume an existing keyboard/mouse may share the bus; support address reassignment so the KVM device can coexist without blocking local input.

## CDC test interface (third CDC channel)

- Add **CDC2** in `usb_descriptors.c` and update TUSB configuration.
- In `app_core`, add a small terminal parser:
  - Arrow keys (escape sequences): update mouse X/Y deltas.
  - `!` toggles the mouse button state (temporary until true key up/down tracking is added).
  - Auto-trigger a ~30s ROM-boot key hold once after the first decoded ADB command byte to verify bus activity without manual input.
  - Other characters: map to ADB keyboard keycodes and enqueue a press/release.
- Keep this in a separate `adb_test_cdc.c/.h` module for cleanliness.

## File/module breakdown

**New modules (proposed):**

- `src/adb_pio.pio` / `src/adb_pio.c/.h`
  - PIO program definition, loader, and helper APIs.
- `src/adb_bus.c/.h`
  - Core1-facing ADB bus logic (command parse, timing state, SRQ control).
- `src/adb_kbd.c/.h`
  - Keyboard register model + event queue.
- `src/adb_mouse.c/.h`
  - Mouse register model + event queue.
- `src/adb_core.c/.h`
  - Core1 dispatch loop wrapper that integrates with `kvm_core` and PIO ISR.
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
- Keep ADB service time in core1 (KVMCore) to a tight budget and avoid blocking operations.
- Use lightweight queues to minimize lock contention and avoid interfering with the existing frame TX queue.
- Instrument core1 utilization around ADB work so we can confirm we stay well within the remaining budget (KVMCore reports ~12% usage currently).

## Implementation steps

1. **PIO prototype**: draft `adb_pio.pio` for line sampling + bit emission; validate timing in isolation.
2. **Core1 ADB engine**: implement bus state machine (`adb_bus`) with command parsing and SRQ emission.
3. **Keyboard/mouse models**: implement register logic and event queues, including default addresses/handler IDs and Listen/Flush behaviors.
4. **Core bridge**: add ADB event queues and event injection APIs on core0.
5. **CDC test channel**: add CDC2 descriptors, parse arrow keys and `!` toggle, map to ADB events.
6. **Integrate with main**: init ADB modules alongside KVMCore; ensure idle states do not affect capture.
7. **Instrumentation + docs**: add optional diagnostics for SRQ/traffic and update docs (protocol + ADB plan/log/notes).

## First bring-up attempt (initial ADB test)

- **Hardware sanity**:
- Confirm ADB line level shifting: 74LVC245 on GPIO6 (RECV) + ULN2803 open-collector on GPIO14 (XMIT).
  - Ensure shared ADB data line with external pull-up and common ground; do not drive high directly.
- **PIO1 wiring**:
  - Load RX + TX programs on PIO1, with RX SM sampling the RECV pin and TX SM toggling XMIT output enable.
  - Run ADB PIO at clkdiv=8 so RX tick counts convert cleanly to microseconds for pulse filtering.
  - Verify that TX is idle (line released) unless a Talk response or SRQ is scheduled.
- **Core1 timing loop**:
  - Schedule the ADB service loop to run every ~50–70 µs and parse attention/sync/bit cells.
  - Gate local TX echo unless explicitly in loopback validation mode.
- **Core0 bridge + CDC2**:
  - Add CDC2 descriptors and parser so arrow keys/`!`/typed characters enqueue ADB events.
  - Emit a rate-limited “ADB RX seen” line when valid host traffic is decoded.
- **On-bus validation**:
  - On a real Mac, watch for ADB reset + Talk polling; confirm SRQ pulses when events are queued.
  - Cross-check bit timing and register contents against `/opt/adb` references during scope validation.

## Test plan

- **Basic bus health**: scope the ADB line for attention/sync, Talk responses, and SRQ pulses.
- **Terminal-driven test**: open CDC2 from Linux and verify:
  - Arrow keys move the Mac cursor.
  - `!` toggles the mouse button.
  - Typed characters appear on the Mac.
- **Collision/address resolution**: connect a real ADB keyboard or mouse and ensure our device reassigns or coexists properly.
- **Performance**: check core0/core1 utilization counters while exercising ADB input + full-rate video capture.

## Open questions / follow-ups

- Confirm exact mouse report format (classic vs extended mouse) before final register definitions.
- Confirm keyboard handler ID and LED behavior match expected Mac OS defaults.
- Validate SRQ timing windows against the ADB spec once the Apple spec PDF is available.

# ADB peripheral implementation strategy

## Current approach: External ADB microcontroller

After exploring on-Pico ADB implementation, we've pivoted to using an **external dedicated ADB microcontroller** (ATmega328p running the MacFriends Arduino core) communicating with the RP2040 via UART1. This approach offers several advantages:

### Rationale

1. **Resource allocation**: The RP2040's PIO, DMA, and interrupt resources are heavily utilized for video capture, which is the primary function of this device. Adding ADB bus handling creates resource contention and timing conflicts.

2. **Timing isolation**: ADB requires precise real-time bus timing that can conflict with video capture DMA and USB servicing. A dedicated microcontroller eliminates these conflicts.

3. **Proven implementations**: The MacFriends Arduino core already provides a working ADB keyboard/mouse bridge with a known-good host client.

4. **Simpler integration**: UART1 communication between the Pico and the ADB controller avoids PIO resource juggling while keeping the RP2040’s capture pipeline isolated.

### Architecture

```
┌─────────────┐ UART1  ┌──────────────┐  ADB Bus  ┌──────────┐
│  RP2040     │◄──────►│ ATmega328p   │◄─────────►│  Mac     │
│  (Pico)     │        │  ADB Stack   │           │  Classic │
│             │        │              │           │          │
│ - Video     │        │ - Keyboard   │           │          │
│ - USB Host  │        │ - Mouse      │           │          │
└─────────────┘        └──────────────┘           └──────────┘
```

### Current focus

For now, we are focusing on:

1. **Improving video capture performance**: DMA postprocessing, non-blocking operations, batched frame transmission
2. **Stable USB enumeration**: Proper tud_task() servicing, correct interrupt priorities
3. **Reliable frame streaming**: VSYNC raw IRQ handling, optimized core1 utilization

### Future work: External ADB integration

When we're ready to add ADB support:

1. Flash the ATmega328p with the MacFriends Arduino core.
2. Mirror the MacFriends host client’s UART protocol for keyboard/mouse events.
3. Add UART1 transport support to Pico firmware.
4. Design hardware interface (UART level shifting, connector pinout, ADB shield wiring).
5. Integrate with USB HID on the Pico to translate host keyboard/mouse to UART1 commands.

### Reference materials

Previous exploration of on-Pico ADB implementation identified these key resources:

- **ADB Manager PDF**: Device addressing, handler IDs, register protocol
- **AN591B (Microchip)**: Timing specifications, bus waveforms
- **macfriends**: Arduino core + host client for ADB keyboard/mouse over USB serial
- **hootswitch**: RP2040 PIO + DMA ADB implementation (host + device)
- **trabular**: AVR ADB keyboard + mouse emulation firmware (alternate reference)
- **adb-usb**: Simple ADB host implementation
- **Lopaciuk ADB protocol summary**: Clear protocol walk-through and timing summary (https://www.lopaciuk.eu/2021/03/26/apple-adb-protocol.html)
- **TMK Apple Desktop Bus wiki**: ADB overview and implementation notes used by multiple firmware projects (https://github.com/tmk/tmk_keyboard/wiki/Apple-Desktop-Bus)
- **MiSTer plus_too ADB core**: Verilog device-side ADB implementation for reference (https://raw.githubusercontent.com/mist-devel/plus_too/refs/heads/master/adb.v)

These remain valuable for the external controller approach.

## ADB Device Inventory

### In-repo MacFriends snapshot (Arduino core)

| Device | ADB address | Registers used | Notes | Key files |
| --- | --- | --- | --- | --- |
| Keyboard | 0x2 | Talk Register 0 (keycode), Talk Register 2 (modifiers/LEDs) | Implements SRQ/pending handling; Register 3 is stubbed for address assignment. | `Arduino/src/adb.cpp`, `Arduino/include/adb.h`, `Arduino/src/main.cpp` |
| Mouse | 0x3 | Talk Register 0 (two motion bytes) | Implements SRQ/pending handling. | `Arduino/src/adb.cpp`, `Arduino/include/adb.h`, `Arduino/src/main.cpp` |

### External repos referenced in `docs/agent/notes.md`

| Repo | Device coverage (from README) | URL | Key files/paths to review |
| --- | --- | --- | --- |
| trabular | Extended keyboard, standard mouse, and an arbitrary device slot. | https://github.com/saybur/trabular | `README.md`, `adb.c`, `registers.c`, `serial.c`, `main.c`, `data.h` |
| tashtrio | Keyboard, mouse, and Global Village TelePort A300 ADB modem. | https://github.com/lampmerchant/tashtrio | `README.md`, `firmware/tashtrio.asm` |
| adb-test-device | Demo devices plus a TelePort A300 modem class. | https://github.com/lampmerchant/adb-test-device | `README.md`, `a300.py`, `testdevice.py`, `testdevice.asm` |
| adbuino | USB/PS2 → ADB keyboard and mouse bridge. | https://github.com/akuker/adbuino | `README.md`, `src/firmware/src`, `src/firmware/include` |
| QuokkADB-firmware | USB → ADB keyboard and mouse bridge (RP2040). | https://github.com/rabbitholecomputing/QuokkADB-firmware | `README.md`, `QuokkADB.md`, `src/firmware/src`, `src/firmware/include` |
| HIDHopper_ADB | USB → ADB keyboard and mouse bridge (RP2040/Pico). | https://github.com/TechByAndroda/HIDHopper_ADB | `README.md`, `doc/`, `src/firmware/src`, `src/firmware/include` |
| adb-usb | Host-side ADB keyboard to USB converter (not device emulation). | https://github.com/gblargg/adb-usb | `README.md`, `adb.c`, `main.c`, `usb_keyboard.c` |

### Spec-defined devices (Apple docs)

The ADB Manager documentation and the HW01 technote enumerate device classes and handler-ID expectations that define what a “standard” device should look like, even if we do not yet emulate them:

- **Keyboards**, **mouse devices**, **tablets (graphics tablets)**, and **low-speed ADB modems** are documented device classes with default address/handler ID tables in the ADB Manager reference. The doc also calls out handler-ID behavior such as Apple Standard keyboard (handler ID 1) and Apple Extended keyboard (handler ID 2) and the ability to switch handler IDs via Listen Register 3.  
- **Trackballs**, **absolute pointing devices (tablets)**, and **extended mouse protocol** behavior are described in HW01, alongside the classic mouse protocol and device-class fields for pointing devices.

## Archived: On-Pico implementation notes

The detailed on-Pico implementation plan has been archived. Key findings:

- **Hardware wiring**: UART1 on GPIO20/21 (Pico ↔ ATmega328p), with a resistor divider on Pico RX
- **PIO placement**: PIO1 was selected to avoid PIO0 video capture conflicts
- **Timing requirements**: ~100µs attention pulse, 65-85µs bit cells
- **Integration challenges**: IRQ priority conflicts with USB, PIO resource pressure, DMA channel contention

These findings informed the decision to move ADB handling to an external controller.

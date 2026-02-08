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

These remain valuable for the external controller approach.

## Archived: On-Pico implementation notes

The detailed on-Pico implementation plan has been archived. Key findings:

- **Hardware wiring**: UART1 on GPIO20/21 (Pico ↔ ATmega328p), with a resistor divider on Pico RX
- **PIO placement**: PIO1 was selected to avoid PIO0 video capture conflicts
- **Timing requirements**: ~100µs attention pulse, 65-85µs bit cells
- **Integration challenges**: IRQ priority conflicts with USB, PIO resource pressure, DMA channel contention

These findings informed the decision to move ADB handling to an external controller.

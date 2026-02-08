# ADB peripheral implementation strategy

## Current approach: External ADB microcontroller

After exploring on-Pico ADB implementation, we've pivoted to using an **external dedicated ADB microcontroller** (ATtiny85 or similar) communicating with the RP2040 via SPI. This approach offers several advantages:

### Rationale

1. **Resource allocation**: The RP2040's PIO, DMA, and interrupt resources are heavily utilized for video capture, which is the primary function of this device. Adding ADB bus handling creates resource contention and timing conflicts.

2. **Timing isolation**: ADB requires precise real-time bus timing that can conflict with video capture DMA and USB servicing. A dedicated microcontroller eliminates these conflicts.

3. **Proven implementations**: Mature ADB firmware for AVR microcontrollers (like ATtiny85) already exists and is well-tested (e.g., trabular firmware).

4. **Simpler integration**: SPI communication between the Pico and ADB controller provides a clean interface without complex interrupt priority management or PIO resource juggling.

### Architecture

```
┌─────────────┐  SPI   ┌──────────────┐  ADB Bus  ┌──────────┐
│  RP2040     │◄──────►│  ATtiny85    │◄─────────►│  Mac     │
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

1. Select appropriate AVR microcontroller (ATtiny85 or similar)
2. Port/adapt existing ADB device firmware (trabular or similar)
3. Design SPI protocol for keyboard/mouse events
4. Add SPI master support to Pico firmware
5. Design hardware interface (level shifting if needed, connector pinout)
6. Integrate with USB HID on the Pico to translate host keyboard/mouse to ADB

### Reference materials

Previous exploration of on-Pico ADB implementation identified these key resources:

- **ADB Manager PDF**: Device addressing, handler IDs, register protocol
- **AN591B (Microchip)**: Timing specifications, bus waveforms
- **trabular**: AVR ADB keyboard + mouse emulation firmware (`/opt/adb/trabular`)
- **trabatar**: Example of driving trabular over a serial link (`/opt/adb/trabatar`)
- **adb-usb**: Simple ADB host implementation

These remain valuable for the external controller approach.

## Archived: On-Pico implementation notes

The detailed on-Pico implementation plan has been archived. Key findings:

- **Hardware wiring**: GPIO6 (ADB RECV), GPIO12 (ADB XMIT, inverted)
- **PIO placement**: PIO1 was selected to avoid PIO0 video capture conflicts
- **Timing requirements**: ~100µs attention pulse, 65-85µs bit cells
- **Integration challenges**: IRQ priority conflicts with USB, PIO resource pressure, DMA channel contention

These findings informed the decision to move ADB handling to an external controller.

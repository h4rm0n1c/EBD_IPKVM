# ADB peripheral integration (trabular)

## Current approach: External ADB microcontroller

We use an **external dedicated ADB microcontroller** (ATtiny85) running the
canonical trabular firmware. The RP2040 talks to the ATtiny85 over SPI using
trabular's byte-oriented command protocol so the Pico never has to meet ADB
bus timing directly.

### Rationale

1. **Resource allocation**: The RP2040's PIO, DMA, and interrupt resources are heavily utilized for video capture, which is the primary function of this device. Adding ADB bus handling creates resource contention and timing conflicts.

2. **Timing isolation**: ADB requires precise real-time bus timing that can conflict with video capture DMA and USB servicing. A dedicated microcontroller eliminates these conflicts.

3. **Proven implementation**: trabular already implements the ADB keyboard,
   mouse, and arbitrary-device registers on AVR and is well-tested.

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

### SPI interface (trabular)

- Each SPI byte is split into a command nibble (upper 4 bits) and a payload
  nibble (lower 4 bits), matching trabular's `handle_serial_data()`.
- Responses are returned one transfer later; the Pico must send a dummy byte
  to clock out the response (e.g., `0x00` after a status request).
- The ATtiny85 polls the USI overflow flag from its main loop, so the Pico
  must pace transfers (~50 µs or slower between bytes).

See the upstream trabular source in `/opt/adb/trabular` for the authoritative
SPI/USART protocol and ADB register behavior.

### Current focus

For now, we are focusing on:

1. **Improving video capture performance**: DMA postprocessing, non-blocking operations, batched frame transmission
2. **Stable USB enumeration**: Proper tud_task() servicing, correct interrupt priorities
3. **Reliable frame streaming**: VSYNC raw IRQ handling, optimized core1 utilization

### Reference materials

- **trabular** (ATtiny85 ADB firmware): `/opt/adb/trabular`
- **ADB Manager PDF / AN591B**: `/opt/adb/miscdocs`

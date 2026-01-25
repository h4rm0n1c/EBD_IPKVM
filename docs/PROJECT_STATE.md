# Project state (living)

## Goal
Macintosh Classic KVM:
- Capture raw TTL video signals: PIXCLK + HSYNC + VSYNC + 1bpp VIDEO
- RP2040 PIO+DMA capture → stream to a host/web UI
- Future: ADB keyboard+mouse emulation, ATX soft power, reset/NMI

## Current behavior (firmware)
- GPIO pin mapping:
  - `GPIO0` PIXCLK (PIO input)
  - `GPIO1` VSYNC (SIO GPIO input, active-low, IRQ on falling edge)
  - `GPIO2` HSYNC (PIO input, active-low)
  - `GPIO3` VIDEO (PIO input)
  - `GPIO9` ATX `PS_ON` (output via ULN2803, GPIO high asserts PSU on)
- Signal conditioning:
  - PIXCLK and VIDEO are buffered through a 74HC14 Schmitt-trigger inverter before reaching the Pico inputs.
  - Firmware defaults to sampling PIXCLK on the falling edge and inverts captured VIDEO bits to restore polarity.
- Capture window:
  - VSYNC falling edge arms a frame if `armed` and not already capturing.
  - Skips 28 HSYNC lines (vertical blank), captures 342 active lines.
- Each line waits for the selected HSYNC edge, skips 178 PIXCLK cycles, then samples 512 bits.
- Throughput controls:
  - Alternates frames on each VSYNC to target ~30 fps.
  - Stops after 100 transmitted frames until reset.
- USB CDC streaming:
  - Lines buffered in a 512-entry ring buffer (72 bytes/packet).
  - Packets are fixed-size and headered (`0xEB 0xD1`).
  - Host must send `S` to arm, `X` to stop, `R` to reset counters, `Q` to park.
  - Edge testing: `H` toggles HSYNC edge, `K` toggles PIXCLK edge, `V` toggles VSYNC edge (stops capture + clears queue).
  - Power/control: `P` asserts ATX `PS_ON`, `p` deasserts it, `B` enters BOOTSEL, `Z` watchdog resets firmware.

## Host tooling
- `src/host_recv_frames.py` is the host-side test program; it reads CDC packets and emits PGM frames.
- Script expects 512×342 frames and writes `frames/frame_###.pgm` by default.

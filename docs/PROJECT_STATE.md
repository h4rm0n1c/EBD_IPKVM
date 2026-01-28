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
- Capture window:
  - VSYNC falling edge arms a frame if `armed` and not already capturing.
  - Skips 28 HSYNC lines (vertical blank), captures 342 active lines.
- Each line waits for the selected HSYNC edge, skips 157 PIXCLK cycles, then samples 512 bits.
- Throughput controls:
  - Captures every VSYNC to target ~60 fps.
  - Runs continuously until stopped/reset.
- UDP RLE streaming:
  - Each line is RLE-compressed and sent as a UDP datagram (`0xEB 0xD1` header).
  - Host must send `S` to arm, `X` to stop, `R` to reset counters, `Q` to park (CDC control channel).
  - Power/control: `P` asserts ATX `PS_ON`, `p` deasserts it, `B` enters BOOTSEL, `Z` watchdog resets firmware.
  - Edge testing: `V` toggles VSYNC edge (stops capture + clears state).
  - Wi-Fi configuration defaults to a captive portal AP (`EBD-IPKVM-Setup`) until credentials are saved.
  - The HTTP config server remains available in station mode for live tweaks and power control (DNS/DHCP stay AP-only).
  - `W` clears stored Wi-Fi settings and reboots into portal mode.

## Host tooling
- `src/host_recv_udp.py` is the host-side test program; it reads UDP RLE packets and emits PGM frames (or relays to VLC).
- Script expects 512×342 frames and writes `frames/frame_###.pgm` by default.
- `scripts/cdc_cmd.py` sends CDC command bytes (for example, `I` or `G`) and prints ASCII responses.
- `scripts/ab_capture.py` runs two capture passes over CDC (legacy), toggling VIDEO inversion between runs (requires firmware support for the `O` command).

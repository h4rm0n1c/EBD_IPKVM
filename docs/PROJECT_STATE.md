# Project state (living)

## Goal
Macintosh Classic KVM:
- Capture raw TTL video signals: PIXCLK + HSYNC + VSYNC + 1bpp VIDEO
- RP2040 PIO+DMA capture → stream to a host/web UI
- Future: External ADB keyboard+mouse (ATmega328p via UART1), ATX soft power, reset/NMI

## Current behavior (firmware)
- GPIO pin mapping:
  - `GPIO0` PIXCLK (PIO input)
  - `GPIO1` VSYNC (SIO GPIO input, active-low, IRQ on falling edge)
  - `GPIO2` HSYNC (PIO input, active-low)
  - `GPIO3` VIDEO (PIO input)
  - `GPIO9` ATX `PS_ON` (output via ULN2803, GPIO high asserts PSU on)
  - `GPIO20` UART1 TX to external ADB controller (ATmega328p)
  - `GPIO21` UART1 RX from external ADB controller (ATmega328p, via resistor divider)
- Capture window:
  - VSYNC falling edge arms a frame if `armed` and not already capturing.
  - Skips 28 HSYNC lines (vertical blank), captures 342 active lines.
- Each line waits for the selected HSYNC edge, skips 157 PIXCLK cycles, then samples 512 bits.
- Throughput controls:
  - Default continuous mode streams every VSYNC (~60 fps) and runs until stopped (toggle with `M`).
  - Test mode alternates frames on each VSYNC to target ~30 fps.
- USB streaming and control:
  - Video lines stream over the vendor bulk interface (headered with `0xEB 0xD1`).
  - Control/debug stays on CDC (`S` arm, `X` stop, `R` reset counters, `Q` park).
  - Edge testing: `H` toggles HSYNC edge, `K` toggles PIXCLK edge, `V` toggles VSYNC edge (stops capture + clears queue).
  - Mode toggle: `M` switches between test and continuous capture cadence.
  - Power/control: `P` asserts ATX `PS_ON`, `p` deasserts it, `B` enters BOOTSEL, `Z` watchdog resets firmware.
- AppleCore (core1) handles time-sensitive video capture with optimized DMA postprocessing and non-blocking frame transmission, while core0 manages USB/CDC and app logic.

## Host tooling
- `src/host_recv_frames.py` is the host-side test program; it reads bulk packets and emits PGM frames by default (use `--pbm` for packed 1-bpp output).
- Script expects 512×342 frames and writes `frames/frame_###.pgm` by default.
- `scripts/cdc_cmd.py` sends CDC command bytes (for example, `I` or `G`) and prints ASCII responses.
- `scripts/ab_capture.py` runs two capture passes, toggling VIDEO inversion between runs (requires firmware support for the `O` command).
- GIF helper (PBM/PGM frames): `ffmpeg -framerate 30 -i frame_%03d.pbm -vf "palettegen" palette.png` then `ffmpeg -framerate 30 -i frame_%03d.pbm -i palette.png -lavfi paletteuse output.gif` (swap `.pgm` if using `--pgm`).

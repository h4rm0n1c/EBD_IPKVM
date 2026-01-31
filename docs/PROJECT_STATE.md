# Project state (living)

## Goal
Macintosh Classic KVM:
- Capture raw TTL video signals: PIXCLK + HSYNC + VSYNC + 1bpp VIDEO
- RP2040 PIO+DMA capture → stream to a host/web UI
- In progress: ADB keyboard+mouse emulation (see `docs/adb_implementation_plan.md` for bring-up steps)
- Planned: ATX soft power, reset/NMI

## Current behavior (firmware)
- GPIO pin mapping:
  - `GPIO0` PIXCLK (PIO input)
  - `GPIO1` VSYNC (SIO GPIO input, active-low, IRQ on falling edge)
  - `GPIO2` HSYNC (PIO input, active-low)
  - `GPIO3` VIDEO (PIO input)
  - `GPIO9` ATX `PS_ON` (output via ULN2803, GPIO high asserts PSU on)
  - `GPIO6` ADB RECV (input via 74LVC245 from Mac ADB data)
  - `GPIO14` ADB XMIT (output via ULN2803 to Mac ADB data, open-collector)
- Capture window:
  - VSYNC falling edge arms a frame if `armed` and not already capturing.
  - Skips 28 HSYNC lines (vertical blank), captures 342 active lines.
- Each line waits for the selected HSYNC edge, skips 157 PIXCLK cycles, then samples 512 bits.
- Throughput controls:
  - Default continuous mode streams every VSYNC (~60 fps) and runs until stopped (toggle with `M`).
  - Test mode alternates frames on each VSYNC to target ~30 fps.
- USB CDC streaming:
  - Lines buffered in a 512-entry ring buffer (72 bytes/packet).
  - Packets are fixed-size and headered (`0xEB 0xD1`).
  - Host must send `S` to arm, `X` to stop, `R` to reset counters, `Q` to park.
  - Edge testing: `H` toggles HSYNC edge, `K` toggles PIXCLK edge, `V` toggles VSYNC edge (stops capture + clears queue).
  - Mode toggle: `M` switches between test and continuous capture cadence.
  - Power/control: `P` asserts ATX `PS_ON`, `p` deasserts it, `B` enters BOOTSEL, `Z` watchdog resets firmware.
  - CDC interfaces: CDC0 stream, CDC1 control/status, CDC2 ADB test input (arrow keys for mouse, `!` toggles button).

## Host tooling
- `src/host_recv_frames.py` is the host-side test program; it reads CDC packets and emits PBM frames (use `--pgm` for 8-bit output).
- Script expects 512×342 frames and writes `frames/frame_###.pbm` by default.
- `scripts/cdc_cmd.py` sends CDC command bytes (for example, `I` or `G`) and prints ASCII responses.
- `scripts/ab_capture.py` runs two capture passes, toggling VIDEO inversion between runs (requires firmware support for the `O` command).
- GIF helper (PBM/PGM frames): `ffmpeg -framerate 30 -i frame_%03d.pbm -vf "palettegen" palette.png` then `ffmpeg -framerate 30 -i frame_%03d.pbm -i palette.png -lavfi paletteuse output.gif` (swap `.pgm` if using `--pgm`).

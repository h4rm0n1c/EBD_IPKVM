# EBD_IPKVM - Everything But Disks, IP KVM

![DEMO](docs/output.gif)

Work-in-progress “IP KVM” for a Macintosh Classic using an RP2040 (Pico W):
Tap raw 1-bpp video + sync, capture with PIO+DMA, and stream to a host UI.

Above output is current project output as of PR #19.

## Highlights
- RP2040 PIO captures 512 pixels per line (1 bpp) on PIXCLK edges.
- Lines are queued and streamed over USB CDC in fixed-size packets.
- Host test helper (`src/host_recv_frames.py`) reconstructs frames into PGM images.

## Signal/pin map (current firmware)
- `GPIO0` — PIXCLK (input, PIO)
- `GPIO1` — VSYNC (input, SIO GPIO, active-low, IRQ on falling edge)
- `GPIO2` — HSYNC (input, PIO, active-low)
- `GPIO3` — VIDEO (input, PIO, 1 bpp data)
- `GPIO9` — ATX `PS_ON` (output via ULN2803, GPIO high asserts PSU on)

⚠️ Upstream signals may be 5V TTL; ensure proper level shifting before the Pico.

## Capture geometry
- Active video: 512×342 (1 bpp)
- Horizontal offset: 178 PIXCLK cycles after the selected HSYNC edge (PIO skip loop)
- Vertical offset: 28 HSYNCs after VSYNC fall
- Capture window: 370 HSYNCs total (28 VBL + 342 active)
- Line capture begins on the selected HSYNC edge.

## USB stream (CDC)
Each line is emitted as a 72-byte packet:

```
0..1  magic      0xEB 0xD1
2..3  frame_id   little-endian
4..5  line_id    little-endian (0..341)
6..7  payload_len (bytes, currently 64)
8..71 line payload (64 bytes = 512 packed pixels)
```

See `docs/protocol/usb_cdc_stream.md` for the full description and host framing notes.

## Host capture helper

```bash
python3 src/host_recv_frames.py /dev/ttyACM0 frames
```

This test script:
- Waits briefly after opening the CDC port (default 0.25s) to allow Pico boot.
- Sends `X` to stop any prior run before arming (use `--no-stop` to skip).
- Resets counters and arms capture by sending `R` then `S` (use `--no-reset` to skip).
- Adjust the boot wait with `--boot-wait=SECONDS` if needed.
- Use `--diag-secs=SECONDS` to briefly print ASCII status before arming capture.
- Reassembles lines into full 512×342 frames.
- Writes PGM files to `frames/` (0/255 grayscale).
- Edge toggles for testing: send `H` to flip HSYNC edge, `K` to flip PIXCLK edge, `V` to flip VSYNC edge (capture stops/clears when toggled).

## Repo layout
- `src/` firmware sources (Pico SDK)
  - `classic_line.pio`: PIO program for per-line capture.
  - `main.c`: USB CDC streaming firmware.
  - `host_recv_frames.py`: host-side test program for reassembling frames.
- `docs/`
  - `PROJECT_STATE.md`: living status summary.
  - `protocol/`: wire format documentation.
  - `agent/`: running notes + decisions for Codex/agents.
  - PDFs: helper datasheets used during bring-up.

## Build (typical Pico SDK)
Set your Pico SDK path, then build out-of-tree in `build/`.

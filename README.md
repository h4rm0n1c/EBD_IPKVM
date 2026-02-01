# EBD_IPKVM - Everything But Disks, IP KVM

![DEMO](docs/output.gif)

Work-in-progress “IP KVM” for a Macintosh Classic using an RP2040 (Pico W):
Tap raw 1-bpp video + sync, capture with PIO+DMA, and stream to a host UI.

Above output is current project output as of PR #19.

## Highlights
- RP2040 PIO captures 512 pixels per line (1 bpp) on PIXCLK edges.
- Lines are queued and streamed over USB CDC with a compact per-line header (optional RLE).
- Host test helper (`src/host_recv_frames.py`) reconstructs frames into PGM images (default).

## Signal/pin map (current firmware)
- `GPIO0` — PIXCLK (input, PIO)
- `GPIO1` — VSYNC (input, SIO GPIO, active-low, IRQ on falling edge)
- `GPIO2` — HSYNC (input, PIO, active-low)
- `GPIO3` — VIDEO (input, PIO, 1 bpp data)
- `GPIO6` — ADB RECV (input via 74LVC245 from Mac ADB data)
- `GPIO14` — ADB XMIT (output via ULN2803 to Mac ADB data, open-collector)
- `GPIO9` — ATX `PS_ON` (output via ULN2803, GPIO high asserts PSU on)

⚠️ Upstream signals may be 5V TTL; ensure proper level shifting before the Pico.

## Capture geometry
- Active video: 512×342 (1 bpp)
- Horizontal offset: 175 PIXCLK cycles after the selected HSYNC edge (PIO skip + 18-cycle delay after phase-lock)
- Vertical offset: 28 HSYNCs after VSYNC fall
- Capture window: 370 HSYNCs total (28 VBL + 342 active)
- Line capture begins on the selected HSYNC edge.

## USB CDC interfaces
The firmware exposes three CDC interfaces:
- **CDC0 (stream)**: binary line packets (video data).
- **CDC1 (control)**: ASCII commands + status text.
- **CDC2 (ADB test)**: keyboard/mouse test input (arrow keys for mouse, `!` toggles button).

On Linux, prefer `/dev/serial/by-id/*EBD_IPKVM*if00|if02|if04` instead of
`/dev/ttyACM*` so enumeration changes do not break scripts. Use
`udevadm info -n /dev/ttyACM2 | rg "ID_MODEL|ID_SERIAL|ID_USB_INTERFACE_NUM"`
to map a new `/dev/ttyACM*` node to the correct interface number.

See `docs/protocol/usb_cdc_stream.md` for details on interfaces and commands.

### Stream packet format (CDC0)
Each line is emitted as a packet with a compact header:

```
0..1   magic       0xEB 0xD1
2..3   frame_id    little-endian
4..5   line_id     little-endian (0..341)
6..7   payload_len little-endian (bit 15 indicates RLE)
8..N   line payload (64 bytes raw, or up to 128 bytes RLE)
```

RLE payloads expand to 64 bytes on the host; firmware may still emit raw
lines if RLE does not compress.

## Host capture helper

```bash
python3 src/host_recv_frames.py /dev/ttyACM0 frames --ctrl-device=/dev/ttyACM1
```

This test script:
- Uses the stream/control CDC ports (override with `--stream-device=` and `--ctrl-device=`).
- Optionally asserts `P` (power on) before capture; use `--no-boot` to skip.
- Sends `X` to stop any prior run before arming (use `--no-stop` to skip).
- Resets counters and arms capture by sending `R` then `S` (use `--no-reset` to skip).
- Adjust the boot wait with `--boot-wait=SECONDS` if needed.
- Use `--diag-secs=SECONDS` to briefly print ASCII status before arming capture.
- Reassembles lines into full 512×342 frames.
- Writes PGM files to `frames/` (0/255 grayscale) by default; use `--pbm` for packed 1-bpp PBM.
- Optionally emits a continuous 8-bit raw stream with `--stream-raw` or `--stream-raw=/path/to/pipe` (runs until you stop it).
- Firmware defaults to continuous ~60 fps capture; send `M` to toggle to the ~30 fps test cadence.
- Edge toggles for testing: send `H` to flip HSYNC edge, `K` to flip PIXCLK edge, `V` to flip VSYNC edge (capture stops/clears when toggled).

Example: stream raw 512×342 8-bit frames to ffplay on stdout:

```bash
python3 src/host_recv_frames.py /dev/ttyACM0 frames --stream-raw \
  | ffplay -f rawvideo -pixel_format gray -video_size 512x342 -framerate 60 -
```

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

## Attribution
This project relies on documentation and reference implementations from the following sources.

### Classic Mac video timing + signaling references (docs/)
- [**Classic Macintosh Video Signals Demystified, Designing a Mac-to-VGA Adapter with LM1881** (Big Mess o' Wires)](docs/mac_classic_video_protocol/Classic%20Macintosh%20Video%20Signals%20Demystified,%20Designing%20a%20Mac-to-VGA%20Adapter%20with%20LM1881%20_%20Big%20Mess%20o%27%20Wires.pdf) — monitor ID pins, composite sync behavior, and sync-on-green details for classic Macs.
- [**Control a Macintosh Classic CRT with a BeagleBone Black (Part 1)**](docs/mac_classic_video_protocol/Control%20a%20Macintosh%20Classic%20CRT%20with%20a%20BeagleBone%20Black%20-%20Part%201%20_%20Nerdhut.pdf) — Classic CRT HSYNC/VSYNC/DATA timing reference.
- [**Mac SE/30 video interface**](docs/mac_classic_video_protocol/Mac-SE_30%20video%20interface%20-%20Trammell%20Hudson%27s%20Projects.pdf) — SE/30 timing notes and capture window context.
- [**Macintosh Classic II Developer Note**](docs/mac_classic_video_protocol/mac_classic_ii.pdf) — 512×342 timing chart and dot clock reference.
- [**Mac Plus Analog Board**](docs/mac_classic_video_protocol/plus_analog.pdf) — sync polarity + line rate notes for compact Macs.
- **Datasheets**: [SP3222E](docs/SP3222E.PDF), [SN74LS245](docs/sn74ls245.pdf), [ULN2803A](docs/uln2803a.pdf) — electrical interface references used during bring-up.

### /opt references
- **/opt/MacDevDocs** — Apple developer notes + hardware references used to cross-check classic Mac timing and connector pinouts (see the [Apple Developer Documentation Archive](https://developer.apple.com/library/archive/)).
- [**/opt/Pico-SDK**](https://github.com/raspberrypi/pico-sdk) — RP2040 PIO/DMA/USB SDK structure and build expectations.
- [**/opt/PicoHTTPServer**](https://github.com/sysprogs/PicoHTTPServer) — Pico W HTTP server, captive portal flow, and incremental response patterns for future on-device UI work.
- [**/opt/SigrokPico**](https://github.com/pico-coder/sigrok-pico) — USB CDC transport patterns and RLE compression ideas for low-entropy line data.
- [**/opt/picovga**](https://github.com/codaris/picovga-cmake) — multi-core video pipeline patterns for RP2040 video workloads.

## Build (typical Pico SDK)
Set your Pico SDK path, then build out-of-tree in `build/`.

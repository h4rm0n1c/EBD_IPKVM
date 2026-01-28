# EBD_IPKVM - Everything But Disks, IP KVM

![DEMO](docs/output.gif)

Work-in-progress “IP KVM” for a Macintosh Classic using an RP2040 (Pico W):
Tap raw 1-bpp video + sync, capture with PIO+DMA, and stream to a host UI.

Above output is current project output as of PR #17.

## Highlights
- RP2040 PIO captures 512 pixels per line (1 bpp) on PIXCLK edges.
- Lines are RLE-compressed and streamed over UDP on Pico W Wi-Fi.
- Wi-Fi configuration is handled by a built-in captive portal (AP mode) when unconfigured.
- Host test helpers reconstruct frames into PGM images or relay them into VLC.

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

## UDP stream (RLE)
Each line is emitted as a UDP datagram containing a compact header and RLE payload:

```
0..1  magic      0xEB 0xD1
2     version    0x01
3     format     0x01 (RLE over 64 packed bytes)
4..5  frame_id   little-endian
6..7  line_id    little-endian (0..341)
8..9  payload_len (bytes, RLE stream)
10..  payload    RLE pairs (count, value)
```

See `docs/protocol/udp_rle_stream.md` for the full description and host framing notes.
USB CDC remains available for control commands (start/stop/reset/power).

## Wi-Fi setup (captive portal)
On first boot (or after clearing settings), the Pico W starts an AP named
`EBD-IPKVM-Setup`. Connect with a phone/laptop and open any page to reach the
setup portal (DNS redirects to the config page). Use the form to set:
- Wi-Fi SSID/password
- UDP target IP/port for video streaming

To factory-reset the stored Wi-Fi settings, send `W` over the CDC control
channel (the device reboots into portal mode).

When running in station mode, the same HTTP server stays available on the
device IP for live control/config tweaks (DNS/DHCP services remain AP-only),
including power-on/off controls.

## Host capture helper

```bash
python3 src/host_recv_udp.py --bind 0.0.0.0 --port 5004 --outdir frames
```

Relay to VLC (rawvideo over UDP):

```bash
python3 src/host_recv_udp.py --bind 0.0.0.0 --port 5004 --vlc-host 127.0.0.1 --vlc-port 6000
vlc --demux rawvideo --rawvid-width 512 --rawvid-height 342 --rawvid-fps 60 \
    --rawvid-chroma GREY udp://@:6000
```

This test script:
- Listens for UDP RLE line packets and reassembles lines into full 512×342 frames.
- Writes PGM files to `frames/` (0/255 grayscale) when `--outdir` is provided.
- Optionally relays reconstructed frames to VLC as 8-bit grayscale rawvideo.

## Repo layout
- `src/` firmware sources (Pico SDK)
  - `classic_line.pio`: PIO program for per-line capture.
  - `main.c`: UDP RLE streaming firmware.
  - `host_recv_udp.py`: host-side UDP RLE receiver/relay.
- `docs/`
  - `PROJECT_STATE.md`: living status summary.
  - `protocol/`: wire format documentation.
  - `agent/`: running notes + decisions for Codex/agents.
  - PDFs: helper datasheets used during bring-up.

## Build (typical Pico SDK)
Set your Pico SDK path, then build out-of-tree in `build/`. The project now
defaults to `PICO_BOARD=pico_w` so Wi-Fi headers/libs are available without
extra flags.

Wi-Fi settings are stored in flash. Compile-time defaults can still be provided
to prefill the portal:
- `WIFI_SSID` / `WIFI_PASSWORD`
- `VIDEO_UDP_ADDR` / `VIDEO_UDP_PORT`

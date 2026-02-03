# USB line stream protocol (bulk + CDC)

The firmware exposes one vendor bulk interface for streaming, two CDC
interfaces for status/ADB testing, and EP0 vendor control transfers for capture
control:

- BULK0 (vendor): video stream (binary packets).
- CDC1: status + limited control (ASCII commands and logs).
- CDC2: ADB test input (ASCII keystrokes and mouse motion).

Captured Macintosh Classic video is streamed as fixed-size packets over the
vendor bulk interface. The bulk endpoint is dedicated to the binary line
stream; capture control uses EP0 vendor requests while status stays on CDC1.
Each packet contains a single scanline of 512 pixels (1 bpp) and a compact
header for framing.

### Identifying bulk stream vs CDC on Linux
The USB interface strings are set to `EBD_IPKVM stream (bulk)`, `EBD_IPKVM control`,
and `EBD_IPKVM adb test`, which are visible in tools like `lsusb -v` or
`udevadm info -a`. The kernel only exposes CDC interfaces under
`/dev/serial/by-id`, so the stream endpoint is accessed via libusb/pyusb.
`src/host_recv_frames.py` uses pyusb by default when `--stream-device=usb`.

- `...-if02` → CDC1 (control)
- `...-if04` → CDC2 (ADB test input)

On Linux, vendor-specific bulk interfaces typically show `usbfs` as the driver in
`lsusb -t`. This is expected because no class driver is bound for the stream;
libusb/pyusb talks to the bulk endpoints through usbfs.

## Packet layout (variable length)

| Offset | Size | Field | Notes |
| ------ | ---- | ----- | ----- |
| 0      | 1    | magic0 | `0xEB` |
| 1      | 1    | magic1 | `0xD1` |
| 2      | 2    | frame_id | Little-endian frame counter (increments per transmitted frame). |
| 4      | 2    | line_id | Little-endian line index (0..341). |
| 6      | 2    | payload_len | Little-endian payload length (bytes). Bit 15 set indicates RLE payload. |
| 8      | 64..128 | payload | Raw 1 bpp pixels (64 bytes) or RLE data (up to 128 bytes). |

### Payload format
- Each line is 512 pixels → 512 bits → 64 bytes.
- Bits are packed MSB-first; bit 7 is leftmost within each byte.
- Firmware byte-swaps each 32-bit word from the PIO RX FIFO before enqueueing, so the payload is already in byte order for host unpacking.
- Host expansion examples: see `src/host_recv_frames.py` (`bytes_to_row64`).
- If bit 15 of `payload_len` is set, the payload is byte-wise RLE encoded as `(count, value)` pairs (count 1..255) and should expand to 64 bytes.
- Firmware may emit raw packets even when RLE mode is enabled if the RLE payload is not smaller than 64 bytes.

## CDC1 control/status commands
CDC1 now carries status output and limited control only (non-capture).

| Command | Action |
| ------- | ------ |
| `R` | Reset counters and internal state. |
| `P` | Assert ATX `PS_ON` (power on; GPIO9 high via ULN2803). |
| `p` | Deassert ATX `PS_ON` (power off; GPIO9 low via ULN2803). |
| `B` | Reboot into BOOTSEL USB mass storage (RP2040 boot ROM). |
| `Z` | Reboot the RP2040 firmware (watchdog reset). |
| `I` | Emit a one-line debug summary of internal CDC/capture state. |

Status lines (including utilization counters) are emitted on CDC1 and can be
read without interfering with the bulk video stream. Utilization percentages
(`c0`, `c1`) reflect time spent doing actual USB handling, capture, and TX queue
work (only when those operations perform work), rather than total loop
occupancy.

## EP0 vendor control requests (capture)
Capture control is issued via EP0 vendor control transfers (host → device):

- `bmRequestType`: `0x40` (vendor, host-to-device, device recipient)
- `wValue`: `0`
- `wIndex`: `0`
- `wLength`: `0`
- `bRequest`: command ID below

| bRequest | Command | Action |
| --- | --- | --- |
| `0x01` | `CAPTURE_START` | Arm capture (begin reacting to VSYNC). |
| `0x02` | `CAPTURE_STOP` | Stop capture, clear TX queue. |
| `0x03` | `RESET_COUNTERS` | Reset counters and internal state. |
| `0x04` | `PROBE_PACKET` | Emit a single probe packet (fixed payload) for sanity checking. |
| `0x05` | `RLE_ON` | Enable RLE line encoding (raw packets still possible if they are smaller). Default. |
| `0x06` | `RLE_OFF` | Disable RLE line encoding (force raw 64-byte payloads). |
| `0x07` | `CAPTURE_PARK` | Park (stop capture and idle forever until reset). |

`src/host_recv_frames.py` defaults to EP0 control transfers for capture commands
and can be forced back to CDC with `--ctrl-cdc` if needed for legacy testing.

## CDC2 ADB test input
CDC2 is a development-only input channel for ADB keyboard/mouse testing. It
accepts simple ASCII input and arrow-key escape sequences to enqueue ADB events
for the core1 ADB service loop.

| Input | Action |
| --- | --- |
| Arrow keys (`ESC [ A/B/C/D`) | Mouse move (up/down/right/left). |
| `Ctrl+R` (`0x12`) | Toggle primary mouse button and emit a button event. |
| Printable ASCII, Tab, Backspace, Delete | Emit a key press + release. |
| Enter (`\\r`/`\\n`) | Emit a Return key press + release. |

## Capture cadence
- Streams every VSYNC (~60 fps).
- Frames are only marked for transmit when the TX path is idle (no queued packets, no pending frame-ready, and no in-flight frame), which prevents backpressure from skipping frame IDs.
- VSYNC IRQs are debounced in firmware (edges closer than 8ms are ignored) to filter glitch pulses and stabilize frame boundaries.
- VSYNC arms the next capture window; the current capture finishes when the fixed-length DMA transfer completes (not on the VSYNC edge).
- Each frame is captured into a ping-pong framebuffer via a single fixed-length DMA transfer (no per-line DMA IRQs); line packets are assembled from that buffer in the main loop (outside IRQ).
- Line capture begins on the HSYNC falling edge before the horizontal skip window for classic capture timing.
- PIXCLK is phase-locked after HSYNC so the first capture edge is deterministic (avoids 1-pixel phase slips); capture samples on PIXCLK rising edges with a small post-edge delay before sampling.
- After the 157-PIXCLK horizontal skip (XOFF), the PIO waits an additional 18 PIXCLK cycles before sampling to shift the active capture window away from the left blanking porch.
- Capture DMA is sized for `CAP_MAX_LINES` (YOFF+ACTIVE) and runs to completion; payloads normally use `CAP_YOFF_LINES + line_id` when indexing into the captured buffer, but if the captured frame is short the firmware falls back to the last `CAP_ACTIVE_H` lines.
- Streaming runs until stopped in both modes.

## Error handling
- If the TX queue is full, line packets are dropped and `lines_drop` increments.
- If USB write fails or buffer is full, `usb_drops` increments.
- If a frame finishes while the previous frame is still queued for transmit, the older ready frame is dropped and `frame_overrun` increments (see debug/status output).
- If a frame contains fewer than `CAP_ACTIVE_H` captured lines, the frame is skipped and `frame_short` increments.

For concrete host-side parsing and reassembly, refer to
`src/host_recv_frames.py`.

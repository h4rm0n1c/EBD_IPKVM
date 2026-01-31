# USB CDC line stream protocol

The firmware exposes three USB CDC interfaces:

- CDC0: video stream (binary packets).
- CDC1: control + status (ASCII commands and logs).
- CDC2: ADB test input (keyboard/mouse event injection).

Captured Macintosh Classic video is streamed as fixed-size packets over CDC0.
Each packet contains a single scanline of 512 pixels (1 bpp) and a compact
header for framing.

### Identifying CDC0 vs CDC1 on Linux
The USB interface strings are set to `EBD_IPKVM stream`, `EBD_IPKVM control`,
and `EBD_IPKVM adb`,
which are visible in tools like `lsusb -v` or `udevadm info -a`. The kernel
also exposes per-interface symlinks in `/dev/serial/by-id` using the interface
number:

- `...-if00` → CDC0 (stream)
- `...-if02` → CDC1 (control)
- `...-if04` → CDC2 (ADB test)

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

## Host control commands
The firmware is host-controlled over CDC1 (control channel):

| Command | Action |
| ------- | ------ |
| `S` | Arm capture (begin reacting to VSYNC). |
| `X` | Stop capture, clear TX queue. |
| `R` | Reset counters and internal state. |
| `Q` | Park (stop capture and idle forever until reset). |
| `P` | Assert ATX `PS_ON` (power on; GPIO9 high via ULN2803). |
| `p` | Deassert ATX `PS_ON` (power off; GPIO9 low via ULN2803). |
| `B` | Reboot into BOOTSEL USB mass storage (RP2040 boot ROM). |
| `Z` | Reboot the RP2040 firmware (watchdog reset). |
| `G` | Report GPIO input states and edge counts over a short sampling window. |
| `F` | Force a capture window immediately (bypasses VSYNC gating for one frame). |
| `T` | Transmit a synthetic test frame (alternating black/white lines) and emit a probe packet. |
| `U` | Emit a single probe packet (fixed payload) for raw CDC sanity checking. |
| `I` | Emit a one-line debug summary of internal CDC/capture state. |
| `V` | Toggle VSYNC edge (fall↔rise), stop capture, and reset the line queue. |
| `M` | Toggle capture cadence between ~30 fps test mode (100-frame cap) and continuous ~60 fps streaming. |
| `E` | Enable RLE line encoding (raw packets still possible if they are smaller). Default. |
| `e` | Disable RLE line encoding (force raw 64-byte payloads). |

Status lines (including utilization counters) are emitted on CDC1 and can be
read without interfering with the CDC0 video stream. Utilization percentages
(`c0`, `c1`) reflect time spent doing actual USB handling, capture, and TX queue
work (only when those operations perform work), rather than total loop
occupancy.

ADB status is emitted on CDC1 as:

- `[EBD_IPKVM] adb rx=<filtered> raw=<total> last=<us> ev=<events> drop=<drops>`
  - `rx` counts pulses that pass the ADB pulse-width filter (µs window).
  - `raw` counts all observed low pulses, even if they are too short/long.
  - `last` is the most recent observed low-pulse width in microseconds.

## ADB test channel (CDC2)
- ANSI arrow keys inject mouse deltas (default 5 counts per press).
- `!` toggles the mouse button state.
- Other printable characters are mapped to ADB keycodes and queued as press/release pairs.

### GPIO diagnostic output (`G`)
- Emitted on CDC1 (control channel).
- Temporarily samples GPIO states and counts transitions for PIXCLK/HSYNC/VSYNC/VIDEO.
- Edge counts are sampled (polling-based), so very high-frequency signals can undercount; they are intended to confirm activity, not exact frequency.
- Output format:
  - `[EBD_IPKVM] gpio diag: pixclk=<0|1> hsync=<0|1> vsync=<0|1> video=<0|1> edges/<secs> pixclk=<count> hsync=<count> vsync=<count> video=<count>`

## Capture cadence
- Default mode streams every VSYNC (~60 fps).
- Test mode toggles `want_frame` every VSYNC to reduce output to ~30 fps.
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

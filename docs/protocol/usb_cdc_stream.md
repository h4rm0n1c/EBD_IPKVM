# USB CDC line stream protocol

The firmware streams captured Macintosh Classic video as fixed-size packets over
USB CDC. Each packet contains a single scanline of 512 pixels (1 bpp) and a
compact header for framing.

## Packet layout (72 bytes)

| Offset | Size | Field | Notes |
| ------ | ---- | ----- | ----- |
| 0      | 1    | magic0 | `0xEB` |
| 1      | 1    | magic1 | `0xD1` |
| 2      | 2    | frame_id | Little-endian frame counter (increments per transmitted frame). |
| 4      | 2    | line_id | Little-endian line index (0..341). |
| 6      | 2    | payload_len | Little-endian payload length (bytes). Currently `64`. |
| 8      | 64   | payload | Packed 1 bpp pixels, MSB-first per byte. |

### Payload format
- Each line is 512 pixels → 512 bits → 64 bytes.
- Bits are packed MSB-first; bit 7 is leftmost within each byte.
- Firmware byte-swaps each 32-bit word from the PIO RX FIFO before enqueueing, so the payload is already in byte order for host unpacking.
- Host expansion examples: see `src/host_recv_frames.py` (`bytes_to_row64`).

## Host control commands
The firmware is host-controlled over the same CDC channel:

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
| `H` | Toggle HSYNC edge (fall↔rise), stop capture, and reset the line queue. |
| `K` | Toggle PIXCLK edge (rise↔fall), stop capture, and reset the line queue. |
| `V` | Toggle VSYNC edge (fall↔rise), stop capture, and reset the line queue. |

### GPIO diagnostic output (`G`)
- Only emitted while capture is stopped and the TX queue is empty.
- Temporarily samples GPIO states and counts transitions for PIXCLK/HSYNC/VSYNC/VIDEO.
- Edge counts are sampled (polling-based), so very high-frequency signals can undercount; they are intended to confirm activity, not exact frequency.
- Output format:
  - `[EBD_IPKVM] gpio diag: pixclk=<0|1> hsync=<0|1> vsync=<0|1> video=<0|1> edges/<secs> pixclk=<count> hsync=<count> vsync=<count> video=<count>`

## Capture cadence
- Firmware toggles `want_frame` every VSYNC to reduce output to ~30 fps.
- Frames are only marked for transmit when the TX path is idle (no queued packets, no pending frame-ready, and no in-flight frame), which prevents backpressure from skipping frame IDs.
- VSYNC IRQs are debounced in firmware (edges closer than 8ms are ignored) to filter glitch pulses and stabilize frame boundaries.
- VSYNC arms the next capture window; the current capture finishes when the fixed-length DMA transfer completes (not on the VSYNC edge).
- Each frame is captured into a ping-pong framebuffer via a single fixed-length DMA transfer (no per-line DMA IRQs); line packets are assembled from that buffer in the main loop (outside IRQ).
- Line capture begins on the selected HSYNC edge before the horizontal skip window.
- PIXCLK is phase-locked after HSYNC so the first capture edge is deterministic (avoids 1-pixel phase slips); rising-edge capture waits for PIXCLK low before the first `wait 1`, and falling-edge capture uses a `wait 1` → `wait 0` → sample sequence per bit.
- Capture DMA is sized for `CAP_MAX_LINES` and is aborted on VSYNC; payloads normally use `CAP_YOFF_LINES + line_id` when indexing into the captured buffer, but if the captured frame is short the firmware falls back to the last `CAP_ACTIVE_H` lines.
- Streaming stops after 100 complete frames unless reset.

## Error handling
- If the TX queue is full, line packets are dropped and `lines_drop` increments.
- If USB write fails or buffer is full, `usb_drops` increments.
- If a frame finishes while the previous frame is still queued for transmit, the older ready frame is dropped and `frame_overrun` increments (see debug/status output).
- If a frame contains fewer than `CAP_ACTIVE_H` captured lines, the frame is skipped and `frame_short` increments.

For concrete host-side parsing and reassembly, refer to
`src/host_recv_frames.py`.

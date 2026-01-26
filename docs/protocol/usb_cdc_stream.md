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
- Firmware deglitches VSYNC by accepting only intervals in the ~12–23 ms range.
- Long gaps reset the VSYNC baseline so capture can recover after missed edges.
- Frames are gated by time so captures occur no more often than ~32 ms (~30 fps).
- Capture automatically aborts if a line window stalls longer than ~50 ms, allowing recovery on the next VSYNC.
- Capture window is 370 HSYNCs total (28 VBL + 342 active).
- Line capture begins on the selected HSYNC edge before the horizontal skip window.
- PIXCLK is phase-locked after HSYNC so the first capture edge is deterministic (avoids 1-pixel phase slips).
- Streaming stops after 100 complete frames unless reset.

## Error handling
- If the TX queue is full, line packets are dropped and `lines_drop` increments.
- If USB write fails or buffer is full, `usb_drops` increments.

For concrete host-side parsing and reassembly, refer to
`src/host_recv_frames.py`.

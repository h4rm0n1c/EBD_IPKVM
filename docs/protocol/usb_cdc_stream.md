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
| `T` | Transmit a synthetic test frame (alternating black/white lines). |
| `U` | Emit a single probe packet (fixed payload) for raw CDC sanity checking. |

### GPIO diagnostic output (`G`)
- Only emitted while capture is stopped and the TX queue is empty.
- Temporarily samples GPIO states and counts transitions for PIXCLK/HSYNC/VSYNC/VIDEO.
- Edge counts are sampled (polling-based), so very high-frequency signals can undercount; they are intended to confirm activity, not exact frequency.
- Output format:
  - `[EBD_IPKVM] gpio diag: pixclk=<0|1> hsync=<0|1> vsync=<0|1> video=<0|1> edges/<secs> pixclk=<count> hsync=<count> vsync=<count> video=<count>`

## Capture cadence
- Firmware toggles `want_frame` every VSYNC to reduce output to ~30 fps.
- Capture window is 370 HSYNCs total (28 VBL + 342 active).
- Streaming stops after 100 complete frames unless reset.

## Error handling
- If the TX queue is full, line packets are dropped and `lines_drop` increments.
- If USB write fails or buffer is full, `usb_drops` increments.

For concrete host-side parsing and reassembly, refer to
`src/host_recv_frames.py`.

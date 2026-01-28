# UDP RLE line stream protocol

The Pico W firmware streams each captured scanline over Wi-Fi using UDP.
Each datagram carries one line of 512 pixels (1 bpp, packed into 64 bytes),
compressed with a simple byte-oriented RLE to reduce bandwidth.

## Packet layout

| Offset | Size | Field | Notes |
| ------ | ---- | ----- | ----- |
| 0      | 1    | magic0 | `0xEB` |
| 1      | 1    | magic1 | `0xD1` |
| 2      | 1    | version | `0x01` |
| 3      | 1    | format | `0x01` = RLE over 64 packed bytes |
| 4      | 2    | frame_id | Little-endian frame counter. |
| 6      | 2    | line_id | Little-endian line index (0..341). |
| 8      | 2    | payload_len | Little-endian payload length (bytes). |
| 10     | N    | payload | RLE pairs (count, value). |

### RLE payload format
- The uncompressed line payload is 64 bytes, representing 512 packed pixels.
- The payload is a sequence of `(count, value)` byte pairs.
- `count` is 1..255 and indicates how many times `value` is repeated.
- The decoder expands pairs until 64 output bytes are produced.

### Packed pixel format
- Each line is 512 pixels → 512 bits → 64 bytes.
- Bits are packed MSB-first; bit 7 is the leftmost pixel within each byte.
- Firmware byte-swaps each 32-bit word from the PIO RX FIFO before encoding, so the payload is already in byte order for host unpacking.

## Host relay for VLC
Use `src/host_recv_udp.py` to reconstruct frames and optionally relay them as
8-bit grayscale rawvideo over UDP so VLC can display the stream:

```bash
python3 src/host_recv_udp.py --bind 0.0.0.0 --port 5004 --vlc-host 127.0.0.1 --vlc-port 6000
vlc --demux rawvideo --rawvid-width 512 --rawvid-height 342 --rawvid-fps 60 \
    --rawvid-chroma GREY udp://@:6000
```

## Wi-Fi configuration
- On boot, if no saved Wi-Fi credentials are present, the device starts an AP
  named `EBD-IPKVM-Setup` and serves a captive portal for configuration.
- The portal lets you set SSID, password, and UDP target IP/port.
- In station mode, the HTTP config server stays available on the device IP
  for live tweaks (DNS/DHCP remain AP-only), including power controls.
- Send CDC command `W` to clear saved credentials and return to portal mode.

## Capture cadence
- Frames are gated to prevent backpressure (capture only starts when the transmit path is idle).
- VSYNC IRQs are debounced in firmware (edges closer than 8ms are ignored) to filter glitch pulses and stabilize frame boundaries.
- Capture DMA is sized for `CAP_MAX_LINES` (YOFF+ACTIVE) and runs to completion; payloads normally use `CAP_YOFF_LINES + line_id` when indexing into the captured buffer, but if the captured frame is short the firmware falls back to the last `CAP_ACTIVE_H` lines.

## Error handling
- If the transmit path is blocked, line packets are dropped and `lines_drop` increments.
- If UDP send fails, `stream_drops` increments.

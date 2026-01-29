#!/usr/bin/env python3
import argparse
import os
import socket
import time
from typing import Optional

W = 512
H = 342
LINE_BYTES = 64

MAGIC0 = 0xEB
MAGIC1 = 0xD1
VERSION = 1
FORMAT_RLE8 = 1
HDR_BYTES = 10


def bytes_to_row64(b: bytes) -> bytes:
    row = bytearray(W)
    i = 0
    for byte in b:
        for bit in range(7, -1, -1):
            row[i] = 255 if (byte >> bit) & 1 else 0
            i += 1
    return bytes(row)


def rle_decode(payload: bytes) -> Optional[bytes]:
    out = bytearray()
    i = 0
    n = len(payload)
    while i + 1 < n and len(out) < LINE_BYTES:
        count = payload[i]
        value = payload[i + 1]
        i += 2
        if count == 0:
            continue
        out.extend([value] * count)
    if len(out) != LINE_BYTES:
        return None
    return bytes(out)


def write_pgm(path: str, rows: list[bytes]) -> None:
    with open(path, "wb") as f:
        f.write(f"P5\n{W} {H}\n255\n".encode("ascii"))
        for r in rows:
            f.write(r)


def parse_packet(data: bytes):
    if len(data) < HDR_BYTES:
        return None
    if data[0] != MAGIC0 or data[1] != MAGIC1:
        return None
    if data[2] != VERSION or data[3] != FORMAT_RLE8:
        return None
    frame_id = data[4] | (data[5] << 8)
    line_id = data[6] | (data[7] << 8)
    payload_len = data[8] | (data[9] << 8)
    if payload_len == 0 or (HDR_BYTES + payload_len) > len(data):
        return None
    payload = data[HDR_BYTES:HDR_BYTES + payload_len]
    return frame_id, line_id, payload


def main() -> None:
    parser = argparse.ArgumentParser(description="Receive UDP RLE video lines and rebuild frames.")
    parser.add_argument("--bind", default="0.0.0.0", help="Bind address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=5004, help="UDP port (default: 5004)")
    parser.add_argument("--pico-host", default="", help="Pico W IP to prime the stream (send one UDP packet)")
    parser.add_argument("--outdir", default="", help="Output directory for PGM frames")
    parser.add_argument(
        "--max-frames",
        type=int,
        default=0,
        help="Stop after N frames (0 = unlimited, default: 0)",
    )
    parser.add_argument("--vlc-host", default="", help="Relay raw 8-bit frames to VLC host")
    parser.add_argument("--vlc-port", type=int, default=6000, help="Relay UDP port (default: 6000)")
    parser.add_argument("--vlc-fps", type=float, default=60.0, help="FPS to advertise in VLC command output")
    parser.add_argument("--vlc-chroma", default="GREY", help="VLC rawvideo chroma (default: GREY)")
    parser.add_argument(
        "--reprime-seconds",
        type=float,
        default=2.0,
        help="Re-send the pico prime packet after N seconds without UDP data (0 disables)",
    )
    parser.add_argument(
        "--stall-seconds",
        type=float,
        default=2.0,
        help="Drop incomplete frames after N seconds without new lines (0 disables)",
    )
    args = parser.parse_args()

    if args.outdir:
        os.makedirs(args.outdir, exist_ok=True)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    sock.settimeout(0.5)

    vlc_sock = None
    vlc_target = None
    if args.vlc_host:
        vlc_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        vlc_target = (args.vlc_host, args.vlc_port)

    frames = {}
    order = []
    counts = {}
    done = 0
    last_print = time.time()
    last_packet_time = time.time()
    last_prime_time = 0.0
    frame_last_update = {}

    print(f"[udp] listening on {args.bind}:{args.port}")
    if args.pico_host:
        sock.sendto(b"EBD_IPKVM", (args.pico_host, args.port))
        last_prime_time = time.time()
        print(f"[udp] primed stream via {args.pico_host}:{args.port}")
    else:
        print("[udp] pico host not set; no stream prime packet sent")
    if vlc_target:
        print("[udp] VLC command:")
        print(
            "  vlc --demux rawvideo --rawvid-width {w} --rawvid-height {h} "
            "--rawvid-fps {fps:.2f} --rawvid-chroma {chroma} udp://@:{port}".format(
                w=W,
                h=H,
                fps=args.vlc_fps,
                chroma=args.vlc_chroma,
                port=args.vlc_port,
            )
        )

    def should_stop() -> bool:
        return args.max_frames > 0 and done >= args.max_frames

    while not should_stop():
        try:
            data, _addr = sock.recvfrom(2048)
        except socket.timeout:
            now = time.time()
            if (
                args.pico_host
                and args.reprime_seconds > 0
                and now - last_packet_time > args.reprime_seconds
                and now - last_prime_time > args.reprime_seconds
            ):
                sock.sendto(b"EBD_IPKVM", (args.pico_host, args.port))
                last_prime_time = now
                print(f"[udp] re-primed stream via {args.pico_host}:{args.port}")
            if args.stall_seconds > 0:
                stalled = [
                    frame_id
                    for frame_id in order
                    if now - frame_last_update.get(frame_id, now) > args.stall_seconds
                ]
                for frame_id in stalled:
                    have = counts.get(frame_id, 0)
                    print(
                        "[udp] dropping stalled frame_id={} lines={}/{} (> {:.1f}s)".format(
                            frame_id,
                            have,
                            H,
                            args.stall_seconds,
                        )
                    )
                    frames.pop(frame_id, None)
                    counts.pop(frame_id, None)
                    frame_last_update.pop(frame_id, None)
                    if frame_id in order:
                        order.remove(frame_id)
            if now - last_print > 1.0:
                last_print = now
                if order:
                    newest = order[-1]
                    have = counts.get(newest, 0)
                    limit = args.max_frames if args.max_frames > 0 else "∞"
                    print(f"[udp] newest frame_id={newest} lines={have}/342 done={done}/{limit}")
                else:
                    limit = args.max_frames if args.max_frames > 0 else "∞"
                    print(f"[udp] done={done}/{limit} (waiting for packets)")
            continue

        parsed = parse_packet(data)
        if not parsed:
            continue
        last_packet_time = time.time()
        frame_id, line_id, payload = parsed
        if line_id >= H:
            continue

        line_bytes = rle_decode(payload)
        if line_bytes is None:
            continue

        frame = frames.get(frame_id)
        if frame is None:
            frame = [None] * H
            frames[frame_id] = frame
            counts[frame_id] = 0
            order.append(frame_id)
            frame_last_update[frame_id] = time.time()
            if len(order) > 4:
                drop = order.pop(0)
                frames.pop(drop, None)
                counts.pop(drop, None)
                frame_last_update.pop(drop, None)

        if frame[line_id] is None:
            frame[line_id] = bytes_to_row64(line_bytes)
            counts[frame_id] += 1
            frame_last_update[frame_id] = time.time()

        if counts[frame_id] == H:
            rows = frame
            if args.outdir:
                out = os.path.join(args.outdir, f"frame_{done:03d}.pgm")
                write_pgm(out, rows)
                print(f"[udp] wrote {out} (frame_id={frame_id})")
            if vlc_sock and vlc_target:
                vlc_sock.sendto(b"".join(rows), vlc_target)
            done += 1
            frames.pop(frame_id, None)
            counts.pop(frame_id, None)
            frame_last_update.pop(frame_id, None)
            if frame_id in order:
                order.remove(frame_id)

    print("[udp] complete.")


if __name__ == "__main__":
    main()

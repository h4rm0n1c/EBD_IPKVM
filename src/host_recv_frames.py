#!/usr/bin/env python3
import os, sys, time, struct, fcntl, termios, select

DEV = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
OUTDIR = sys.argv[2] if len(sys.argv) > 2 else "frames"
MAX_FRAMES = 100

W = 512
H = 342
LINE_BYTES = 64
PKT_BYTES = 2 + 2 + 2 + 2 + LINE_BYTES

MAGIC0 = 0xEB
MAGIC1 = 0xD1

os.makedirs(OUTDIR, exist_ok=True)

def set_raw_and_dtr(fd: int) -> None:
    # Raw mode (no translations, no line discipline surprises)
    attr = termios.tcgetattr(fd)
    attr[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK |
                 termios.ISTRIP | termios.INLCR | termios.IGNCR |
                 termios.ICRNL | termios.IXON)
    attr[1] &= ~termios.OPOST
    attr[2] &= ~(termios.CSIZE | termios.PARENB)
    attr[2] |= termios.CS8
    attr[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON |
                 termios.ISIG | termios.IEXTEN)
    # Non-blocking-ish reads via select(), but keep sane defaults:
    attr[6][termios.VMIN] = 0
    attr[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attr)

    # Assert DTR+RTS so Pico CDC sees "connected"
    TIOCMGET = 0x5415
    TIOCMSET = 0x5418
    TIOCM_DTR = 0x002
    TIOCM_RTS = 0x004
    status = struct.unpack("I", fcntl.ioctl(fd, TIOCMGET, struct.pack("I", 0)))[0]
    status |= (TIOCM_DTR | TIOCM_RTS)
    fcntl.ioctl(fd, TIOCMSET, struct.pack("I", status))

def bytes_to_row64(b: bytes) -> bytes:
    # Expand 64 packed bytes to 512 bytes of 0/255
    row = bytearray(W)
    i = 0
    for byte in b:
        for bit in range(7, -1, -1):
            row[i] = 255 if (byte >> bit) & 1 else 0
            i += 1
    return bytes(row)

def write_pgm(path: str, rows: list[bytes]) -> None:
    with open(path, "wb") as f:
        f.write(f"P5\n{W} {H}\n255\n".encode("ascii"))
        for r in rows:
            f.write(r)

def pop_one_packet(buf: bytearray):
    # find magic
    n = len(buf)
    i = 0
    while i + 1 < n and not (buf[i] == MAGIC0 and buf[i + 1] == MAGIC1):
        i += 1
    if i > 0:
        del buf[:i]
    if len(buf) < PKT_BYTES:
        return None
    pkt = bytes(buf[:PKT_BYTES])
    del buf[:PKT_BYTES]
    return pkt

fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY)
set_raw_and_dtr(fd)

# Tell Pico to start (host-controlled firmware expects this)
os.write(fd, b"S")

print(f"[host] reading {DEV}, writing {OUTDIR}/frame_###.pgm")

buf = bytearray()
frames = {}  # frame_id -> dict(line->row)
done_count = 0
last_print = time.time()

# Optional: If nothing arrives for a while, say so.
last_rx = time.time()

while done_count < MAX_FRAMES:
    r, _, _ = select.select([fd], [], [], 0.25)
    if not r:
        if time.time() - last_rx > 2.0:
            print("[host] no data yet (is Pico armed + Mac running?)")
            last_rx = time.time()
        continue

    chunk = os.read(fd, 8192)
    if not chunk:
        time.sleep(0.01)
        continue

    last_rx = time.time()
    buf.extend(chunk)

    while True:
        pkt = pop_one_packet(buf)
        if pkt is None:
            break

        frame_id = pkt[2] | (pkt[3] << 8)
        line_id  = pkt[4] | (pkt[5] << 8)
        plen     = pkt[6] | (pkt[7] << 8)

        if plen != LINE_BYTES or line_id >= H:
            continue

        payload = pkt[8:8 + LINE_BYTES]
        row = bytes_to_row64(payload)

        fm = frames.setdefault(frame_id, {})
        if line_id not in fm:
            fm[line_id] = row

        if len(fm) == H:
            out = os.path.join(OUTDIR, f"frame_{done_count:03d}.pgm")
            rows = [fm[i] for i in range(H)]
            write_pgm(out, rows)
            print(f"[host] wrote {out} (frame_id={frame_id})")
            done_count += 1
            # free memory for this frame_id
            del frames[frame_id]
            if done_count >= MAX_FRAMES:
                break

    now = time.time()
    if now - last_print > 1.0:
        last_print = now
        if frames:
            newest = max(frames.keys())
            have = len(frames[newest])
            print(f"[host] newest frame_id={newest} lines={have}/342 done={done_count}/100")
        else:
            print(f"[host] done={done_count}/100 (waiting for packets)")

print("[host] complete.")

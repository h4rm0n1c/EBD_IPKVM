#!/usr/bin/env python3
import os, sys, time, struct, fcntl, termios, select

SEND_RESET = True
SEND_STOP = True
SEND_BOOT = True
BOOT_WAIT = 12.0
DIAG_SECS = 12.0
FORCE_AFTER = 2.0
FORCE_START = False
TEST_AFTER = 0.0
TEST_START = False
PROBE_ONLY = False
ARGS = []
for arg in sys.argv[1:]:
    if arg == "--no-reset":
        SEND_RESET = False
    elif arg == "--no-stop":
        SEND_STOP = False
    elif arg == "--no-boot":
        SEND_BOOT = False
    elif arg.startswith("--boot-wait="):
        value = arg.split("=", 1)[1]
        try:
            BOOT_WAIT = float(value)
        except ValueError:
            print(f"[host] invalid --boot-wait value: {value}")
            sys.exit(2)
    elif arg.startswith("--diag-secs="):
        value = arg.split("=", 1)[1]
        try:
            DIAG_SECS = float(value)
        except ValueError:
            print(f"[host] invalid --diag-secs value: {value}")
            sys.exit(2)
    elif arg == "--force-start":
        FORCE_START = True
    elif arg == "--test-frame":
        TEST_START = True
    elif arg == "--probe":
        PROBE_ONLY = True
    elif arg == "--no-force":
        FORCE_AFTER = 0.0
    elif arg == "--no-test":
        TEST_AFTER = 0.0
    elif arg.startswith("--force-after="):
        value = arg.split("=", 1)[1]
        try:
            FORCE_AFTER = float(value)
        except ValueError:
            print(f"[host] invalid --force-after value: {value}")
            sys.exit(2)
    elif arg.startswith("--test-after="):
        value = arg.split("=", 1)[1]
        try:
            TEST_AFTER = float(value)
        except ValueError:
            print(f"[host] invalid --test-after value: {value}")
            sys.exit(2)
    else:
        ARGS.append(arg)

DEV = ARGS[0] if len(ARGS) > 0 else "/dev/ttyACM0"
OUTDIR = ARGS[1] if len(ARGS) > 1 else "frames"
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

if SEND_BOOT:
    os.write(fd, b"P")

if SEND_STOP:
    os.write(fd, b"X")
    time.sleep(0.05)

if DIAG_SECS > 0:
    end_diag = time.time() + DIAG_SECS
    print(f"[host] diag: passively reading ASCII for {DIAG_SECS:.2f}s (no start)")
    diag_buf = bytearray()
    while time.time() < end_diag:
        r, _, _ = select.select([fd], [], [], 0.25)
        if not r:
            continue
        chunk = os.read(fd, 1024)
        if not chunk:
            continue
        diag_buf.extend(chunk)
        while b"\n" in diag_buf:
            line, _, remainder = diag_buf.partition(b"\n")
            diag_buf = bytearray(remainder)
            try:
                text = line.decode("utf-8", errors="replace")
            except UnicodeDecodeError:
                text = repr(line)
            print(f"[host][diag] {text}")

if BOOT_WAIT > 0:
    remaining = BOOT_WAIT - DIAG_SECS
    if remaining > 0:
        time.sleep(remaining)

# Tell Pico to reset counters (optional) then start.
if SEND_RESET:
    os.write(fd, b"R")
    time.sleep(0.05)
if PROBE_ONLY:
    os.write(fd, b"U")
    time.sleep(0.2)
    probe_deadline = time.time() + 1.0
    probe_bytes = 0
    while time.time() < probe_deadline:
        r, _, _ = select.select([fd], [], [], 0.25)
        if not r:
            continue
        chunk = os.read(fd, 8192)
        if chunk:
            probe_bytes += len(chunk)
    print(f"[host] probe bytes received: {probe_bytes}")
    sys.exit(0)
if TEST_START:
    os.write(fd, b"T")
    FORCE_AFTER = 0.0
    FORCE_START = False
elif FORCE_START:
    os.write(fd, b"F")
else:
    os.write(fd, b"S")

mode_note = "reset+start" if SEND_RESET else "start"
boot_note = "boot" if SEND_BOOT else "no-boot"
print(f"[host] reading {DEV}, writing {OUTDIR}/frame_###.pgm ({mode_note}, {boot_note}, boot_wait={BOOT_WAIT:.2f}s, diag={DIAG_SECS:.2f}s)")

buf = bytearray()
frames = {}  # frame_id -> dict(line->row)
done_count = 0
last_print = time.time()

# Optional: If nothing arrives for a while, say so.
last_rx = time.time()
start_rx = last_rx
force_sent = FORCE_START or TEST_START
test_sent = TEST_START

try:
    while done_count < MAX_FRAMES:
        r, _, _ = select.select([fd], [], [], 0.25)
        if not r:
            now = time.time()
            if not force_sent and FORCE_AFTER > 0 and now - start_rx > FORCE_AFTER:
                os.write(fd, b"F")
                force_sent = True
                print(f"[host] no packets yet; sent force-start after {FORCE_AFTER:.2f}s")
            if not test_sent and TEST_AFTER > 0 and now - start_rx > TEST_AFTER:
                os.write(fd, b"T")
                test_sent = True
                print(f"[host] no packets yet; sent test-frame after {TEST_AFTER:.2f}s")
            if now - last_rx > 2.0:
                print("[host] no data yet (is Pico armed + Mac running?)")
                last_rx = now
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
finally:
    if SEND_STOP:
        try:
            os.write(fd, b"X")
        except OSError:
            pass
    try:
        os.write(fd, b"p")
    except OSError:
        pass

print("[host] complete.")

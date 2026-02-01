#!/usr/bin/env python3
import glob, os, sys, time, struct, fcntl, termios, select, errno

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
RLE_MODE = True
OUTPUT_FORMAT = "pgm"
STREAM_RAW = False
STREAM_RAW_PATH = "-"
QUIET = False
QUIET_SET = False
STREAM_DEV = "/dev/serial/by-id/usb-Raspberry_Pi_EBD_IPKVM_E6614C311B855539-if00"
CTRL_DEV = "/dev/serial/by-id/usb-Raspberry_Pi_EBD_IPKVM_E6614C311B855539-if02"
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
    elif arg == "--rle":
        RLE_MODE = True
    elif arg == "--raw":
        RLE_MODE = False
    elif arg == "--no-force":
        FORCE_AFTER = 0.0
    elif arg == "--no-test":
        TEST_AFTER = 0.0
    elif arg == "--pgm":
        OUTPUT_FORMAT = "pgm"
    elif arg == "--pbm":
        OUTPUT_FORMAT = "pbm"
    elif arg == "--stream-raw":
        STREAM_RAW = True
        STREAM_RAW_PATH = "-"
    elif arg.startswith("--stream-raw="):
        STREAM_RAW = True
        STREAM_RAW_PATH = arg.split("=", 1)[1]
    elif arg == "--quiet":
        QUIET = True
        QUIET_SET = True
    elif arg == "--verbose":
        QUIET = False
        QUIET_SET = True
    elif arg.startswith("--stream-device="):
        STREAM_DEV = arg.split("=", 1)[1]
    elif arg.startswith("--ctrl-device="):
        CTRL_DEV = arg.split("=", 1)[1]
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

if len(ARGS) > 0:
    STREAM_DEV = ARGS[0]
OUTDIR = ARGS[1] if len(ARGS) > 1 else "frames"
MAX_FRAMES = None if STREAM_RAW else 100

def resolve_device(path: str) -> str:
    if "*" not in path and "?" not in path and "[" not in path:
        return path
    matches = sorted(glob.glob(path))
    if not matches:
        return path
    return matches[0]

STREAM_DEV = resolve_device(STREAM_DEV)
CTRL_DEV = resolve_device(CTRL_DEV)

W = 512
H = 342
LINE_BYTES = 64
HEADER_BYTES = 8
MAX_PAYLOAD = LINE_BYTES * 2
RLE_FLAG = 0x8000
LEN_MASK = 0x7FFF

MAGIC0 = 0xEB
MAGIC1 = 0xD1

log_out = sys.stdout
raw_stream = None
if STREAM_RAW:
    if not QUIET_SET:
        QUIET = True
    if STREAM_RAW_PATH in ("", "-"):
        raw_stream = sys.stdout.buffer
        log_out = sys.stderr
    else:
        raw_stream = open(STREAM_RAW_PATH, "wb", buffering=0)
else:
    os.makedirs(OUTDIR, exist_ok=True)

def log(message: str) -> None:
    if QUIET:
        return
    print(message, file=log_out)

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

def decode_rle_line(b: bytes):
    if len(b) % 2:
        return None
    out = bytearray()
    i = 0
    while i < len(b):
        count = b[i]
        value = b[i + 1]
        i += 2
        if count == 0:
            return None
        out.extend([value] * count)
        if len(out) > LINE_BYTES:
            return None
    if len(out) != LINE_BYTES:
        return None
    return bytes(out)

def write_pgm(path: str, rows: list[bytes]) -> None:
    with open(path, "wb") as f:
        f.write(f"P5\n{W} {H}\n255\n".encode("ascii"))
        for r in rows:
            f.write(r)

def write_pbm(path: str, rows: list[bytes]) -> None:
    with open(path, "wb") as f:
        f.write(f"P4\n{W} {H}\n".encode("ascii"))
        for r in rows:
            f.write(bytes((~b) & 0xFF for b in r))

def pop_one_packet(buf: bytearray):
    # find magic
    n = len(buf)
    i = 0
    while i + 1 < n and not (buf[i] == MAGIC0 and buf[i + 1] == MAGIC1):
        i += 1
    if i > 0:
        del buf[:i]
    if len(buf) < HEADER_BYTES:
        return None
    plen = buf[6] | (buf[7] << 8)
    payload_len = plen & LEN_MASK
    if payload_len == 0 or payload_len > MAX_PAYLOAD:
        del buf[:2]
        return None
    total_len = HEADER_BYTES + payload_len
    if len(buf) < total_len:
        return None
    pkt = bytes(buf[:total_len])
    del buf[:total_len]
    return pkt

def open_ctrl_fd() -> int:
    fd = os.open(CTRL_DEV, os.O_RDWR | os.O_NOCTTY)
    set_raw_and_dtr(fd)
    return fd

def ensure_ctrl_writable(fd: int, retries: int = 3) -> int:
    for _ in range(retries):
        _, w, _ = select.select([], [fd], [], 0.25)
        if w:
            return fd
    return fd

def ctrl_write(fd: int, payload: bytes) -> int:
    try:
        os.write(fd, payload)
        return fd
    except OSError as exc:
        if exc.errno not in (errno.EIO, errno.ENXIO, errno.EBADF):
            raise
    log("[host] control port error; reopening ctrl device")
    try:
        os.close(fd)
    except OSError:
        pass
    fd = open_ctrl_fd()
    ensure_ctrl_writable(fd)
    os.write(fd, payload)
    return fd

stream_fd = os.open(STREAM_DEV, os.O_RDWR | os.O_NOCTTY)
ctrl_fd = open_ctrl_fd()
set_raw_and_dtr(stream_fd)

if SEND_BOOT:
    ensure_ctrl_writable(ctrl_fd)
    ctrl_fd = ctrl_write(ctrl_fd, b"P")

if SEND_STOP:
    ensure_ctrl_writable(ctrl_fd)
    ctrl_fd = ctrl_write(ctrl_fd, b"X")
    time.sleep(0.05)

if DIAG_SECS > 0:
    end_diag = time.time() + DIAG_SECS
    log(f"[host] diag: passively reading ASCII for {DIAG_SECS:.2f}s (no start)")
    diag_buf = bytearray()
    while time.time() < end_diag:
        r, _, _ = select.select([ctrl_fd], [], [], 0.25)
        if not r:
            continue
        chunk = os.read(ctrl_fd, 1024)
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
            log(f"[host][diag] {text}")

if BOOT_WAIT > 0:
    remaining = BOOT_WAIT - DIAG_SECS
    if remaining > 0:
        time.sleep(remaining)

# Tell Pico to reset counters (optional) then start.
if SEND_RESET:
    ensure_ctrl_writable(ctrl_fd)
    ctrl_fd = ctrl_write(ctrl_fd, b"R")
    time.sleep(0.05)
if RLE_MODE is True:
    ensure_ctrl_writable(ctrl_fd)
    ctrl_fd = ctrl_write(ctrl_fd, b"E")
    time.sleep(0.01)
elif RLE_MODE is False:
    ensure_ctrl_writable(ctrl_fd)
    ctrl_fd = ctrl_write(ctrl_fd, b"e")
    time.sleep(0.01)
if PROBE_ONLY:
    ensure_ctrl_writable(ctrl_fd)
    ctrl_fd = ctrl_write(ctrl_fd, b"U")
    time.sleep(0.2)
    probe_deadline = time.time() + 1.0
    probe_bytes = 0
    while time.time() < probe_deadline:
        r, _, _ = select.select([stream_fd], [], [], 0.25)
        if not r:
            continue
        chunk = os.read(stream_fd, 8192)
        if chunk:
            probe_bytes += len(chunk)
    print(f"[host] probe bytes received: {probe_bytes}")
    sys.exit(0)
if TEST_START:
    ensure_ctrl_writable(ctrl_fd)
    ctrl_fd = ctrl_write(ctrl_fd, b"T")
    FORCE_AFTER = 0.0
    FORCE_START = False
elif FORCE_START:
    ensure_ctrl_writable(ctrl_fd)
    ctrl_fd = ctrl_write(ctrl_fd, b"F")
else:
    ensure_ctrl_writable(ctrl_fd)
    ctrl_fd = ctrl_write(ctrl_fd, b"S")

mode_note = "reset+start" if SEND_RESET else "start"
boot_note = "boot" if SEND_BOOT else "no-boot"
ext = "pbm" if OUTPUT_FORMAT == "pbm" else "pgm"
stream_note = f", raw_stream={STREAM_RAW_PATH}" if STREAM_RAW else ""
if STREAM_RAW:
    log(f"[host] reading {STREAM_DEV}, streaming raw frames ({mode_note}, {boot_note}, boot_wait={BOOT_WAIT:.2f}s, diag={DIAG_SECS:.2f}s{stream_note})")
else:
    log(f"[host] reading {STREAM_DEV}, writing {OUTDIR}/frame_###.{ext} ({mode_note}, {boot_note}, boot_wait={BOOT_WAIT:.2f}s, diag={DIAG_SECS:.2f}s{stream_note})")

buf = bytearray()
frames = {}  # frame_id -> dict(line->row)
frame_stats = {}  # frame_id -> dict(bytes=payload_bytes, rle_lines=count)
done_count = 0
last_print = time.time()

# Optional: If nothing arrives for a while, say so.
last_rx = time.time()
start_rx = last_rx
force_sent = FORCE_START or TEST_START
test_sent = TEST_START

try:
    while True:
        if MAX_FRAMES is not None and done_count >= MAX_FRAMES:
            break
        r, _, _ = select.select([stream_fd], [], [], 0.25)
        if not r:
            now = time.time()
            if not force_sent and FORCE_AFTER > 0 and now - start_rx > FORCE_AFTER:
                ensure_ctrl_writable(ctrl_fd)
                ctrl_fd = ctrl_write(ctrl_fd, b"F")
                force_sent = True
                log(f"[host] no packets yet; sent force-start after {FORCE_AFTER:.2f}s")
            if not test_sent and TEST_AFTER > 0 and now - start_rx > TEST_AFTER:
                ensure_ctrl_writable(ctrl_fd)
                ctrl_fd = ctrl_write(ctrl_fd, b"T")
                test_sent = True
                log(f"[host] no packets yet; sent test-frame after {TEST_AFTER:.2f}s")
            if now - last_rx > 2.0:
                log("[host] no data yet (is Pico armed + Mac running?)")
                last_rx = now
            continue

        chunk = os.read(stream_fd, 8192)
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
            is_rle   = bool(plen & RLE_FLAG)
            payload_len = plen & LEN_MASK

            if payload_len == 0 or payload_len > MAX_PAYLOAD or line_id >= H:
                continue

            payload = pkt[8:8 + payload_len]
            if is_rle:
                decoded = decode_rle_line(payload)
                if decoded is None:
                    continue
                packed = decoded
            else:
                if payload_len != LINE_BYTES:
                    continue
                packed = payload

            fm = frames.setdefault(frame_id, {})
            stats = frame_stats.setdefault(frame_id, {"bytes": 0, "rle_lines": 0})
            if line_id not in fm:
                fm[line_id] = packed
                stats["bytes"] += payload_len
                if is_rle:
                    stats["rle_lines"] += 1

            if len(fm) == H:
                rows = [fm[i] for i in range(H)]
                expanded = None
                if STREAM_RAW or OUTPUT_FORMAT != "pbm":
                    expanded = [bytes_to_row64(row) for row in rows]
                if STREAM_RAW:
                    frame_bytes = b"".join(expanded)
                    raw_stream.write(frame_bytes)
                else:
                    out = os.path.join(OUTDIR, f"frame_{done_count:03d}.{ext}")
                    if OUTPUT_FORMAT == "pbm":
                        write_pbm(out, rows)
                    else:
                        write_pgm(out, expanded)
                raw_bytes = LINE_BYTES * H
                payload_bytes = stats["bytes"]
                ratio = payload_bytes / raw_bytes if raw_bytes else 0.0
                percent = ratio * 100.0
                rle_lines = stats["rle_lines"]
                if STREAM_RAW:
                    log(
                        f"[host] streamed frame_id={frame_id} "
                        f"(rle_lines={rle_lines}/{H}, "
                        f"payload_bytes={payload_bytes}, raw_bytes={raw_bytes}, "
                        f"ratio={percent:.1f}%)"
                    )
                else:
                    log(
                        f"[host] wrote {out} (frame_id={frame_id}, "
                        f"rle_lines={rle_lines}/{H}, "
                        f"payload_bytes={payload_bytes}, raw_bytes={raw_bytes}, "
                        f"ratio={percent:.1f}%)"
                    )
                done_count += 1
                # free memory for this frame_id
                del frames[frame_id]
                del frame_stats[frame_id]
                if MAX_FRAMES is not None and done_count >= MAX_FRAMES:
                    break

        now = time.time()
        if now - last_print > 1.0:
            last_print = now
            if frames:
                newest = max(frames.keys())
                have = len(frames[newest])
                log(f"[host] newest frame_id={newest} lines={have}/342 done={done_count}/100")
            else:
                log(f"[host] done={done_count}/100 (waiting for packets)")
finally:
    if SEND_STOP:
        try:
            os.write(ctrl_fd, b"X")
        except OSError:
            pass
    try:
        os.write(ctrl_fd, b"p")
    except OSError:
        pass
    os.close(stream_fd)
    os.close(ctrl_fd)

if raw_stream and raw_stream is not sys.stdout.buffer:
    raw_stream.close()

log("[host] complete.")

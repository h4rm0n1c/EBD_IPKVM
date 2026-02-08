#!/usr/bin/env python3
import os, sys, time, struct, fcntl, termios, select, signal, glob

# Graceful shutdown flag for Ctrl+C
interrupted = False

def signal_handler(signum, frame):
    global interrupted
    interrupted = True

signal.signal(signal.SIGINT, signal_handler)

SEND_RESET = True
SEND_STOP = True
SEND_BOOT = True
BOOT_WAIT = 12.0
DIAG_SECS = 12.0
PROBE_ONLY = False
RLE_MODE = True
OUTPUT_FORMAT = "pgm"
STREAM_RAW = False
STREAM_RAW_PATH = "-"
QUIET = False
QUIET_SET = False
STREAM_DEV = "usb"
CTRL_DEV = None
CTRL_MODE = "ep0"
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
    elif arg == "--probe":
        PROBE_ONLY = True
    elif arg == "--rle":
        RLE_MODE = True
    elif arg == "--raw":
        RLE_MODE = False
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
    elif arg == "--stream-usb":
        STREAM_DEV = "usb"
    elif arg.startswith("--ctrl-device="):
        CTRL_DEV = arg.split("=", 1)[1]
    elif arg == "--ctrl-ep0":
        CTRL_MODE = "ep0"
    elif arg == "--ctrl-cdc":
        CTRL_MODE = "cdc"
    else:
        ARGS.append(arg)

if len(ARGS) > 0:
    STREAM_DEV = ARGS[0]
OUTDIR = ARGS[1] if len(ARGS) > 1 else "frames"
MAX_FRAMES = None if STREAM_RAW else 100

if CTRL_DEV is None:
    CTRL_DEV, err = auto_detect_ctrl_device()
    if err:
        print(err, file=sys.stderr)
        print("[host] supply a control device with --ctrl-device=/dev/ttyACM1.", file=sys.stderr)
        sys.exit(2)
    log(f"[host] auto-detected control CDC device: {CTRL_DEV}")

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

def auto_detect_ctrl_device():
    by_id_dir = "/dev/serial/by-id"
    if not os.path.isdir(by_id_dir):
        return None, f"[host] {by_id_dir} is missing; CDC auto-detect is unavailable."
    entries = sorted(glob.glob(os.path.join(by_id_dir, "*")))
    matches = [e for e in entries if "if01" in os.path.basename(e)]
    if not matches:
        return None, "[host] CDC control port not found (expected /dev/serial/by-id/*if01*)."
    if len(matches) > 1:
        joined = "\n  ".join(matches)
        return None, f"[host] multiple CDC control ports found:\n  {joined}"
    return matches[0], None

def set_cbreak(fd: int):
    attr = termios.tcgetattr(fd)
    new_attr = list(attr)
    new_attr[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON)
    new_attr[3] |= termios.ISIG
    new_attr[6][termios.VMIN] = 0
    new_attr[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, new_attr)
    return attr

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

USB_VID = 0x2E8A
USB_PID = 0x000A

CTRL_REQ_CAPTURE_START = 0x01
CTRL_REQ_CAPTURE_STOP = 0x02
CTRL_REQ_RESET_COUNTERS = 0x03
CTRL_REQ_PROBE_PACKET = 0x04
CTRL_REQ_RLE_ON = 0x05
CTRL_REQ_RLE_OFF = 0x06
CTRL_REQ_CAPTURE_PARK = 0x07

def open_usb_stream():
    try:
        import usb.core
        import usb.util
    except ImportError as exc:
        print(f"[host] pyusb not available: {exc}. Install python3-usb or pyusb.", file=sys.stderr)
        sys.exit(2)

    dev = usb.core.find(idVendor=USB_VID, idProduct=USB_PID)
    if dev is None:
        print("[host] USB device not found (VID/PID 0x2E8A:0x000A).", file=sys.stderr)
        sys.exit(2)

    try:
        cfg = dev.get_active_configuration()
    except usb.core.USBError:
        cfg = None
    if cfg is None:
        try:
            dev.set_configuration()
        except usb.core.USBError as exc:
            if getattr(exc, "errno", None) != 16:
                raise
        cfg = dev.get_active_configuration()
    intf = usb.util.find_descriptor(cfg, bInterfaceClass=0xFF)
    if intf is None:
        print("[host] bulk stream interface not found.", file=sys.stderr)
        sys.exit(2)

    if dev.is_kernel_driver_active(intf.bInterfaceNumber):
        dev.detach_kernel_driver(intf.bInterfaceNumber)

    usb.util.claim_interface(dev, intf.bInterfaceNumber)
    ep_in = usb.util.find_descriptor(
        intf,
        custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN
    )
    if ep_in is None:
        print("[host] bulk IN endpoint not found.", file=sys.stderr)
        sys.exit(2)
    return dev, intf.bInterfaceNumber, ep_in

def open_usb_device_for_control():
    try:
        import usb.core
    except ImportError as exc:
        print(f"[host] pyusb not available: {exc}. Install python3-usb or pyusb.", file=sys.stderr)
        sys.exit(2)

    dev = usb.core.find(idVendor=USB_VID, idProduct=USB_PID)
    if dev is None:
        print("[host] USB device not found (VID/PID 0x2E8A:0x000A).", file=sys.stderr)
        sys.exit(2)

    try:
        dev.get_active_configuration()
    except Exception:
        try:
            dev.set_configuration()
        except Exception:
            pass
    return dev

def send_ep0_cmd(dev, req):
    try:
        # 0x41 = Host-to-Device | Vendor | Interface recipient
        # wIndex=0 targets the vendor bulk interface (ITF_NUM_VENDOR_STREAM).
        # Device-level vendor requests (0x40) may not be routed to
        # tud_vendor_control_xfer_cb by all TinyUSB versions.
        dev.ctrl_transfer(0x41, req, 0, 0, None)
    except Exception as exc:
        print(f"[host] EP0 control transfer failed (req=0x{req:02X}): {exc}", file=sys.stderr)
        sys.exit(2)

def read_usb_stream(ep_in, timeout_s: float) -> bytes:
    timeout_ms = int(timeout_s * 1000)
    try:
        data = ep_in.read(8192, timeout=timeout_ms)
        return bytes(data)
    except Exception:
        return b""

def service_cdc_relay(ready_fds, ctrl_fd, stdin_fd, relay_active):
    if ctrl_fd in ready_fds:
        try:
            chunk = os.read(ctrl_fd, 1024)
        except OSError:
            chunk = b""
        if chunk:
            sys.stderr.buffer.write(chunk)
            sys.stderr.buffer.flush()
    if relay_active and stdin_fd in ready_fds:
        try:
            data = os.read(stdin_fd, 1024)
        except OSError:
            data = b""
        if data:
            os.write(ctrl_fd, data)

use_usb_stream = STREAM_DEV == "usb" or STREAM_DEV.startswith("usb:")
stream_fd = None
usb_dev = None
usb_intf = None
usb_ep_in = None
stdin_fd = None
stdin_attr = None
relay_active = False
if use_usb_stream:
    usb_dev, usb_intf, usb_ep_in = open_usb_stream()
    ctrl_fd = os.open(CTRL_DEV, os.O_RDWR | os.O_NOCTTY)
    set_raw_and_dtr(ctrl_fd)
else:
    stream_fd = os.open(STREAM_DEV, os.O_RDWR | os.O_NOCTTY)
    ctrl_fd = os.open(CTRL_DEV, os.O_RDWR | os.O_NOCTTY)
    set_raw_and_dtr(stream_fd)
    set_raw_and_dtr(ctrl_fd)
    if CTRL_MODE == "ep0":
        usb_dev = open_usb_device_for_control()

if sys.stdin.isatty():
    stdin_fd = sys.stdin.fileno()
    stdin_attr = set_cbreak(stdin_fd)
    relay_active = True

if CTRL_MODE == "ep0" and usb_dev is None:
    print("[host] EP0 control selected but USB device is unavailable.", file=sys.stderr)
    sys.exit(2)

if SEND_BOOT:
    os.write(ctrl_fd, b"P")

if SEND_STOP:
    if CTRL_MODE == "ep0":
        send_ep0_cmd(usb_dev, CTRL_REQ_CAPTURE_STOP)
    else:
        os.write(ctrl_fd, b"X")
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
    if CTRL_MODE == "ep0":
        send_ep0_cmd(usb_dev, CTRL_REQ_RESET_COUNTERS)
    else:
        os.write(ctrl_fd, b"R")
    time.sleep(0.05)
if RLE_MODE is True:
    if CTRL_MODE == "ep0":
        send_ep0_cmd(usb_dev, CTRL_REQ_RLE_ON)
    else:
        os.write(ctrl_fd, b"E")
    time.sleep(0.01)
elif RLE_MODE is False:
    if CTRL_MODE == "ep0":
        send_ep0_cmd(usb_dev, CTRL_REQ_RLE_OFF)
    else:
        os.write(ctrl_fd, b"e")
    time.sleep(0.01)
if PROBE_ONLY:
    if CTRL_MODE == "ep0":
        send_ep0_cmd(usb_dev, CTRL_REQ_PROBE_PACKET)
    else:
        os.write(ctrl_fd, b"U")
    time.sleep(0.2)
    probe_deadline = time.time() + 1.0
    probe_bytes = 0
    while time.time() < probe_deadline:
        if use_usb_stream:
            chunk = read_usb_stream(usb_ep_in, 0.25)
        else:
            r, _, _ = select.select([stream_fd], [], [], 0.25)
            if not r:
                continue
            chunk = os.read(stream_fd, 8192)
        if chunk:
            probe_bytes += len(chunk)
    print(f"[host] probe bytes received: {probe_bytes}")
    sys.exit(0)
if CTRL_MODE == "ep0":
    send_ep0_cmd(usb_dev, CTRL_REQ_CAPTURE_START)
else:
    os.write(ctrl_fd, b"S")

mode_note = "reset+start" if SEND_RESET else "start"
boot_note = "boot" if SEND_BOOT else "no-boot"
ext = "pbm" if OUTPUT_FORMAT == "pbm" else "pgm"
stream_note = f", raw_stream={STREAM_RAW_PATH}" if STREAM_RAW else ""
if STREAM_RAW:
    log(f"[host] reading {STREAM_DEV}, streaming raw frames ({mode_note}, {boot_note}, boot_wait={BOOT_WAIT:.2f}s, diag={DIAG_SECS:.2f}s{stream_note})")
else:
    log(f"[host] reading {STREAM_DEV}, writing {OUTDIR}/frame_###.{ext} ({mode_note}, {boot_note}, boot_wait={BOOT_WAIT:.2f}s, diag={DIAG_SECS:.2f}s{stream_note})")
if relay_active:
    log("[host] CDC relay enabled (stdin -> control, stderr <- CDC; cbreak mode).")
    if STREAM_RAW:
        log("[host] tip: keep ffplay -loglevel quiet so stderr stays readable.")

buf = bytearray()
frames = {}  # frame_id -> dict(line->row)
frame_stats = {}  # frame_id -> dict(bytes=payload_bytes, rle_lines=count)
done_count = 0
last_print = time.time()

# Optional: If nothing arrives for a while, say so.
last_rx = time.time()
start_rx = last_rx
stream_timeout = 0.05 if relay_active else 0.25

try:
    while True:
        if interrupted:
            log("[host] interrupted by user (Ctrl+C)")
            break
        if MAX_FRAMES is not None and done_count >= MAX_FRAMES:
            break
        if use_usb_stream:
            relay_fds = [ctrl_fd]
            if relay_active:
                relay_fds.append(stdin_fd)
            ready, _, _ = select.select(relay_fds, [], [], 0)
            service_cdc_relay(ready, ctrl_fd, stdin_fd, relay_active)
            chunk = read_usb_stream(usb_ep_in, stream_timeout)
            ready, _, _ = select.select(relay_fds, [], [], 0)
            service_cdc_relay(ready, ctrl_fd, stdin_fd, relay_active)
        else:
            fds = [stream_fd, ctrl_fd]
            if relay_active:
                fds.append(stdin_fd)
            ready, _, _ = select.select(fds, [], [], 0.25)
            service_cdc_relay(ready, ctrl_fd, stdin_fd, relay_active)
            if stream_fd in ready:
                chunk = os.read(stream_fd, 8192)
            else:
                chunk = b""

        if not chunk:
            now = time.time()
            if now - last_rx > 2.0:
                log("[host] no data yet (is Pico armed + Mac running?)")
                last_rx = now
            continue
        if not chunk:
            time.sleep(0.01)
            continue

        last_rx = time.time()
        buf.extend(chunk)

        while True:
            if interrupted:
                break
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
    if relay_active and stdin_attr is not None:
        termios.tcsetattr(stdin_fd, termios.TCSANOW, stdin_attr)
    if SEND_STOP:
        try:
            if CTRL_MODE == "ep0":
                send_ep0_cmd(usb_dev, CTRL_REQ_CAPTURE_STOP)
            else:
                os.write(ctrl_fd, b"X")
        except OSError:
            pass
    try:
        os.write(ctrl_fd, b"p")
    except OSError:
        pass
    if stream_fd is not None:
        os.close(stream_fd)
    os.close(ctrl_fd)
    if usb_dev is not None and usb_intf is not None:
        try:
            import usb.util
            usb.util.release_interface(usb_dev, usb_intf)
        except Exception:
            pass

if raw_stream and raw_stream is not sys.stdout.buffer:
    raw_stream.close()

log("[host] complete.")

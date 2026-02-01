#!/usr/bin/env python3
import argparse
import glob
import os
import struct
import subprocess
import sys
import time
import fcntl
import termios
from pathlib import Path


def set_raw_and_dtr(fd: int) -> None:
    attr = termios.tcgetattr(fd)
    attr[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK |
                 termios.ISTRIP | termios.INLCR | termios.IGNCR |
                 termios.ICRNL | termios.IXON)
    attr[1] &= ~termios.OPOST
    attr[2] &= ~(termios.CSIZE | termios.PARENB)
    attr[2] |= termios.CS8
    attr[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON |
                 termios.ISIG | termios.IEXTEN)
    attr[6][termios.VMIN] = 0
    attr[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attr)

    TIOCMGET = 0x5415
    TIOCMSET = 0x5418
    TIOCM_DTR = 0x002
    TIOCM_RTS = 0x004
    status = struct.unpack("I", fcntl.ioctl(fd, TIOCMGET, struct.pack("I", 0)))[0]
    status |= (TIOCM_DTR | TIOCM_RTS)
    fcntl.ioctl(fd, TIOCMSET, struct.pack("I", status))


def resolve_device(path: str) -> str:
    if "*" not in path and "?" not in path and "[" not in path:
        return path
    matches = sorted(glob.glob(path))
    if not matches:
        return path
    return matches[0]


def send_cmd(dev: str, payload: bytes, settle_s: float = 0.1) -> None:
    fd = os.open(resolve_device(dev), os.O_RDWR | os.O_NOCTTY)
    try:
        set_raw_and_dtr(fd)
        os.write(fd, payload)
        time.sleep(settle_s)
    finally:
        os.close(fd)


def run_host_capture(dev: str, ctrl_dev: str, outdir: str, max_frames: int, host_args: list[str]) -> None:
    host_script = Path(__file__).resolve().parent.parent / "src" / "host_recv_frames.py"
    cmd = [sys.executable, str(host_script), dev, outdir,
           f"--max-frames={max_frames}",
           f"--ctrl-device={ctrl_dev}"]
    cmd.extend(host_args)
    print(f"[ab] running: {' '.join(cmd)}")
    subprocess.run(cmd, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="A/B test VIDEO inversion with capture runs.")
    parser.add_argument("--stream-device", default="/dev/serial/by-id/*EBD_IPKVM*if00",
                        help="CDC stream device (CDC0).")
    parser.add_argument("--ctrl-device", default="/dev/serial/by-id/*EBD_IPKVM*if02",
                        help="CDC control device (CDC1).")
    parser.add_argument("--outdir", default="frames_ab", help="Output base directory.")
    parser.add_argument("--max-frames", type=int, default=30, help="Frames per run.")
    parser.add_argument("--settle", type=float, default=1.0, help="Seconds to wait after toggling inversion.")
    parser.add_argument("--host-arg", action="append", default=[], help="Extra arg passed to host_recv_frames.py.")
    args = parser.parse_args()

    if args.max_frames <= 0:
        raise SystemExit("--max-frames must be > 0")

    out_base = Path(args.outdir)
    out_base.mkdir(parents=True, exist_ok=True)

    try:
        run_host_capture(args.stream_device, args.ctrl_device, str(out_base / "A"),
                         args.max_frames, args.host_arg)

        print("[ab] toggling VIDEO inversion for B run")
        send_cmd(args.ctrl_device, b"X")
        send_cmd(args.ctrl_device, b"O")
        time.sleep(args.settle)

        run_host_capture(args.stream_device, args.ctrl_device, str(out_base / "B"),
                         args.max_frames, args.host_arg)
    finally:
        try:
            send_cmd(args.ctrl_device, b"X")
        except OSError:
            pass

    print(f"[ab] done. Compare {out_base / 'A'} vs {out_base / 'B'}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

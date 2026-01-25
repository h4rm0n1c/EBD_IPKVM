#!/usr/bin/env python3
import argparse
import os
import select
import struct
import sys
import time
import fcntl
import termios


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


def main() -> int:
    parser = argparse.ArgumentParser(description="Send a CDC command and print ASCII response.")
    parser.add_argument("--device", default="/dev/ttyACM0", help="CDC device path.")
    parser.add_argument("--cmd", required=True, help="Command byte(s) to send, e.g. I or G.")
    parser.add_argument("--read-secs", type=float, default=2.0, help="Seconds to read responses.")
    args = parser.parse_args()

    if args.read_secs <= 0:
        raise SystemExit("--read-secs must be > 0")

    fd = os.open(args.device, os.O_RDWR | os.O_NOCTTY)
    try:
        set_raw_and_dtr(fd)
        os.write(fd, args.cmd.encode("ascii"))
        end = time.time() + args.read_secs
        buf = bytearray()
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.25)
            if not r:
                continue
            chunk = os.read(fd, 4096)
            if not chunk:
                continue
            buf.extend(chunk)
            while b"\n" in buf:
                line, _, remainder = buf.partition(b"\n")
                buf = bytearray(remainder)
                text = line.decode("utf-8", errors="replace")
                print(text)
        if buf:
            text = buf.decode("utf-8", errors="replace")
            if text.strip():
                print(text)
    finally:
        os.close(fd)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

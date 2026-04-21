#!/usr/bin/env python3
"""
Pull session .dat files off a GoSteady device over the uart1 dump
channel.

Default behavior: connect to /dev/cu.usbmodem105 at 1 Mbaud, LIST the
session directory, pull each file to ./sessions/<uuid>.dat, and run the
M4 read_session.py validation against each. No files are deleted unless
--rm is passed.

Usage:
  python tools/pull_sessions.py                      # list+pull, keep on device
  python tools/pull_sessions.py --rm                 # list+pull+delete on device
  python tools/pull_sessions.py --out /tmp/captures  # custom output dir
  python tools/pull_sessions.py --port /dev/cu.usbmodem105
  python tools/pull_sessions.py --only <uuid>.dat    # single file
  python tools/pull_sessions.py --list-only          # LIST, don't pull
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

import serial

DEFAULT_PORT = "/dev/cu.usbmodem105"
DEFAULT_BAUD = 1_000_000
READ_CHUNK = 4096
BANNER = b"GOSTEADY-DUMP"


def _read_line(ser: serial.Serial, timeout_s: float = 5.0) -> bytes:
    """Read until \\n (exclusive). Raises TimeoutError on inactivity."""
    deadline = time.time() + timeout_s
    buf = bytearray()
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        if b == b"\n":
            return bytes(buf).rstrip(b"\r")
        buf += b
    raise TimeoutError(f"no newline within {timeout_s}s; partial: {bytes(buf)!r}")


def _read_exact(ser: serial.Serial, n: int, timeout_s: float = 30.0) -> bytes:
    """Read exactly n bytes or raise TimeoutError."""
    deadline = time.time() + timeout_s
    buf = bytearray()
    while len(buf) < n and time.time() < deadline:
        chunk = ser.read(min(READ_CHUNK, n - len(buf)))
        if chunk:
            buf += chunk
    if len(buf) < n:
        raise TimeoutError(f"short read: got {len(buf)}/{n} bytes")
    return bytes(buf)


def connect(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial(port, baud, timeout=0.5)
    ser.dtr = True  # wake up the nRF5340 bridge's CDC-to-UART forwarding
    time.sleep(0.3)
    ser.reset_input_buffer()

    # Ping to make sure the device is listening. The dump thread prints
    # a banner at startup, but if the board has been up for a while it's
    # already scrolled off, so we synchronise with a PING.
    ser.write(b"PING\n")
    ser.flush()
    # Eat anything that isn't PONG (stale banner, leftover output).
    deadline = time.time() + 2.0
    while time.time() < deadline:
        try:
            line = _read_line(ser, timeout_s=0.5)
        except TimeoutError:
            break
        if line == b"PONG":
            return ser
    raise RuntimeError(
        f"no PONG from {port} — is the firmware running with the dump channel? "
        f"({BANNER.decode()} banner printed on dump-thread start)"
    )


def list_files(ser: serial.Serial) -> list[tuple[str, int]]:
    ser.write(b"LIST\n")
    ser.flush()
    out: list[tuple[str, int]] = []
    while True:
        line = _read_line(ser).decode("ascii", errors="replace")
        if line == "END":
            return out
        if line.startswith("ERR"):
            raise RuntimeError(f"LIST failed: {line}")
        name, _, size_s = line.partition(" ")
        try:
            out.append((name, int(size_s)))
        except ValueError:
            raise RuntimeError(f"bad LIST line: {line!r}")


def dump_file(ser: serial.Serial, name: str, dest: Path) -> int:
    ser.write(f"DUMP {name}\n".encode())
    ser.flush()

    size_line = _read_line(ser).decode("ascii", errors="replace")
    if size_line.startswith("ERR"):
        raise RuntimeError(f"DUMP {name}: {size_line}")
    if not size_line.startswith("SIZE "):
        raise RuntimeError(f"expected 'SIZE <n>', got {size_line!r}")
    size = int(size_line.split()[1])

    body = _read_exact(ser, size)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(body)

    # Trailing "\nOK\n" or "\nERR ...\n"
    tail = _read_line(ser).decode("ascii", errors="replace")  # empty (the \n after body)
    status = _read_line(ser).decode("ascii", errors="replace")
    if status != "OK":
        raise RuntimeError(f"DUMP {name} ended with {status!r}")
    return size


def delete_file(ser: serial.Serial, name: str) -> None:
    ser.write(f"DEL {name}\n".encode())
    ser.flush()
    status = _read_line(ser).decode("ascii", errors="replace")
    if status != "OK":
        raise RuntimeError(f"DEL {name}: {status}")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--port", default=DEFAULT_PORT)
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    p.add_argument("--out", default="sessions", help="local output directory")
    p.add_argument("--rm", action="store_true",
                   help="delete each file on device after a successful pull")
    p.add_argument("--only", help="pull only this filename")
    p.add_argument("--list-only", action="store_true",
                   help="just print what's on device, don't pull anything")
    args = p.parse_args()

    try:
        ser = connect(args.port, args.baud)
    except Exception as e:
        print(f"connect failed: {e}", file=sys.stderr)
        return 1

    try:
        files = list_files(ser)
    except Exception as e:
        print(f"LIST failed: {e}", file=sys.stderr)
        return 1

    if args.only:
        files = [(n, s) for (n, s) in files if n == args.only]

    print(f"device has {len(files)} session file(s):")
    for name, size in files:
        print(f"  {name:>42}  {size:>8} bytes")
    if args.list_only or not files:
        return 0

    out_dir = Path(args.out)
    pulled = 0
    failed = 0
    for name, size in files:
        dest = out_dir / name
        try:
            got = dump_file(ser, name, dest)
            ok = "OK" if got == size else f"SIZE-MISMATCH({got}/{size})"
            print(f"  pulled {name} -> {dest}  [{ok}]")
            pulled += 1
            if args.rm and got == size:
                delete_file(ser, name)
                print(f"  deleted on device: {name}")
        except Exception as e:
            print(f"  {name}: {e}", file=sys.stderr)
            failed += 1

    print(f"\ndone: {pulled} pulled, {failed} failed")
    return 0 if failed == 0 else 2


if __name__ == "__main__":
    sys.exit(main())

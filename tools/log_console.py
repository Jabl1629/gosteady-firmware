#!/usr/bin/env python3
"""Continuous uart console logger — auto-saves to logs/ for forensic recovery.

Tails one of the two USB CDC endpoints exposed by the nRF5340 connectivity
bridge and writes timestamped lines to a daily rotating log file:

    logs/uart0_YYYY-MM-DD.log     # nRF9151 app console (default)
    logs/uart1_YYYY-MM-DD.log     # protocol channel (--channel uart1)

Why this exists: when the firmware HardFaults or otherwise wedges, the fault
dump prints to uart0 and is gone the moment you scroll past it. Running this
in the background means every fault dump (and every heartbeat preceding it)
is on disk for post-mortem.

Auto-reconnects when the device disappears + reappears (USB replug, power
cycle, brief enumeration glitch). Each connection lifetime gets a header
marker line so you can grep for "OPENED" or "CLOSED" to find boundaries.

  uart0 (115200, /dev/cu.usbmodem*102) is one-way (firmware → host) — safe
    to log continuously without affecting any other tool.

  uart1 (1Mbaud, /dev/cu.usbmodem*105) is bidirectional and shared with
    tools/pull_sessions.py, tools/control.py, and tools/cleanup_device.py.
    Logging it competes with those tools for incoming bytes (each byte goes
    to whichever reader's read() lands first). For most diagnostic work you
    don't need it — capture.html's Log panel already shows the BLE-side
    transcript of all uart1 traffic. Use --channel uart1 only when you
    specifically want to see what the firmware is emitting that the BLE
    client is *not* mirroring (e.g., pre-BLE-connect output).

Usage from repo root:

    # Foreground — Ctrl+C to stop
    /opt/nordic/ncs/toolchains/185bb0e3b6/bin/python3 tools/log_console.py

    # Background — survives terminal close
    nohup /opt/nordic/ncs/toolchains/185bb0e3b6/bin/python3 \\
        tools/log_console.py >/dev/null 2>&1 &

    # Echo to stdout while logging (handy for live tail in another terminal)
    /opt/nordic/ncs/toolchains/185bb0e3b6/bin/python3 \\
        tools/log_console.py --echo
"""

from __future__ import annotations

import argparse
import datetime as dt
import glob
import sys
import time
from pathlib import Path

import serial

REPO_ROOT = Path(__file__).resolve().parent.parent
LOG_DIR = REPO_ROOT / "logs"

CHANNEL_DEFAULTS = {
    "uart0": ("/dev/cu.usbmodem*102", 115200),
    "uart1": ("/dev/cu.usbmodem*105", 1_000_000),
}


def _find_port(pattern: str) -> str | None:
    candidates = sorted(glob.glob(pattern))
    return candidates[0] if candidates else None


def _stamp() -> str:
    return dt.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def _daily_log_path(channel: str) -> Path:
    today = dt.date.today().isoformat()
    return LOG_DIR / f"{channel}_{today}.log"


def _log_session(port: str, baud: int, channel: str, echo: bool) -> bool:
    """Run one connection lifetime. Returns True if disconnect was clean
    (device disappeared), False if Ctrl+C / fatal error in caller."""
    LOG_DIR.mkdir(exist_ok=True)
    try:
        s = serial.Serial(port, baud, timeout=0.5)
    except serial.SerialException as e:
        print(f"[{channel}] open {port} failed: {e}", file=sys.stderr)
        return True
    try:
        s.dtr = True
        time.sleep(0.2)
        s.reset_input_buffer()
        log_path = _daily_log_path(channel)
        with log_path.open("a", buffering=1) as f:
            opened = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            banner = f"\n===== {channel} OPENED on {port} at {opened} ====="
            f.write(banner + "\n")
            if echo:
                print(banner, flush=True)
            partial = b""
            while True:
                try:
                    d = s.read(2048)
                except (serial.SerialException, OSError) as e:
                    f.write(f"[{_stamp()}] !! READ ERROR: {e}\n")
                    break
                if not d:
                    continue
                partial += d
                while b"\n" in partial:
                    line, partial = partial.split(b"\n", 1)
                    text = line.decode("utf-8", errors="replace").rstrip("\r")
                    out = f"[{_stamp()}] {text}"
                    f.write(out + "\n")
                    if echo:
                        print(out, flush=True)
            closed = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            tail = f"===== {channel} CLOSED at {closed} ====="
            f.write(tail + "\n")
            if echo:
                print(tail, flush=True)
        return True
    finally:
        try:
            s.close()
        except Exception:
            pass


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--channel", default="uart0",
                    choices=sorted(CHANNEL_DEFAULTS.keys()),
                    help="Which CDC endpoint to log (default: uart0)")
    ap.add_argument("--port-glob", default=None,
                    help="Override port glob (default depends on channel)")
    ap.add_argument("--baud", type=int, default=None,
                    help="Override baud rate (default depends on channel)")
    ap.add_argument("--echo", action="store_true",
                    help="Also print each line to stdout while logging")
    args = ap.parse_args()

    default_glob, default_baud = CHANNEL_DEFAULTS[args.channel]
    port_glob = args.port_glob or default_glob
    baud = args.baud or default_baud
    log_path = _daily_log_path(args.channel)

    print(f"[{args.channel}] watching {port_glob} @ {baud}")
    print(f"[{args.channel}] logging to {log_path}")
    print("[Ctrl+C to stop]")

    try:
        while True:
            port = _find_port(port_glob)
            if port is None:
                time.sleep(1.0)
                continue
            _log_session(port, baud, args.channel, args.echo)
            # Brief grace period before re-scanning so we don't busy-loop
            # if the device is mid-replug.
            time.sleep(1.0)
    except KeyboardInterrupt:
        print(f"\n[{args.channel}] stopped (logs in {log_path})")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())

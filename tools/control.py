#!/usr/bin/env python3
"""
Drive the GoSteady session-control protocol from the host.

Shares the uart1 channel with tools/pull_sessions.py (same port, same
line protocol). Commands:

  python tools/control.py status
  python tools/control.py start <json-payload>
  python tools/control.py start-preset <preset-name>
  python tools/control.py stop
  python tools/control.py raw "<any line>"

`start-preset` loads a named 12-field payload from the presets dict
below — shortcut for the v1 capture protocol's matrix rows so the
operator doesn't have to hand-type JSON 30 times.

The raw subcommand is an escape hatch for poking the device with
arbitrary lines (LIST, DEL, PING, etc) — useful for diagnostics.
"""

from __future__ import annotations

import argparse
import json
import sys
import time

import serial

DEFAULT_PORT = "/dev/cu.usbmodem105"
DEFAULT_BAUD = 1_000_000


# Subset of the v1 run matrix — enough to sanity-check the protocol
# without having to hand-type a full JSON for every test. Keys match
# the GoSteady_Capture_Protocol_v1.docx "Indoor polished concrete" set.
PRESETS: dict[str, dict] = {
    "bench": {
        "subject_id": "S00", "walker_type": "two_wheel", "cap_type": "glide",
        "walker_model": "unspecified", "mount_config": "front_left_leg",
        "course_id": "bench_m6a", "intended_distance_ft": 0,
        "surface": "polished_concrete", "intended_speed": "normal",
        "direction": "straight", "run_type": "normal", "operator": "bench",
    },
    "concrete-normal-20": {
        "subject_id": "S00", "walker_type": "two_wheel", "cap_type": "glide",
        "walker_model": "unspecified", "mount_config": "front_left_leg",
        "course_id": "concrete_A_straight", "intended_distance_ft": 20,
        "surface": "polished_concrete", "intended_speed": "normal",
        "direction": "straight", "run_type": "normal", "operator": "jace",
    },
    "concrete-slow-20": {
        "subject_id": "S00", "walker_type": "two_wheel", "cap_type": "glide",
        "walker_model": "unspecified", "mount_config": "front_left_leg",
        "course_id": "concrete_A_straight", "intended_distance_ft": 20,
        "surface": "polished_concrete", "intended_speed": "slow",
        "direction": "straight", "run_type": "normal", "operator": "jace",
    },
    "concrete-fast-20": {
        "subject_id": "S00", "walker_type": "two_wheel", "cap_type": "glide",
        "walker_model": "unspecified", "mount_config": "front_left_leg",
        "course_id": "concrete_A_straight", "intended_distance_ft": 20,
        "surface": "polished_concrete", "intended_speed": "fast",
        "direction": "straight", "run_type": "normal", "operator": "jace",
    },
    "concrete-scurve-20": {
        "subject_id": "S00", "walker_type": "two_wheel", "cap_type": "glide",
        "walker_model": "unspecified", "mount_config": "front_left_leg",
        "course_id": "concrete_A_curve", "intended_distance_ft": 20,
        "surface": "polished_concrete", "intended_speed": "normal",
        "direction": "s_curve", "run_type": "normal", "operator": "jace",
    },
}


def connect(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial(port, baud, timeout=0.5)
    ser.dtr = True
    time.sleep(0.3)
    ser.reset_input_buffer()
    # Synchronise with a PING — the dump thread's PONG tells us the
    # channel is live regardless of whatever was mid-stream before.
    ser.write(b"PING\n")
    ser.flush()
    deadline = time.time() + 2.0
    while time.time() < deadline:
        b = ser.readline()
        if not b:
            continue
        if b.rstrip(b"\r\n") == b"PONG":
            return ser
    raise RuntimeError(f"no PONG from {port} — is the firmware running?")


def send(ser: serial.Serial, line: str, timeout_s: float = 3.0) -> str:
    ser.write((line + "\n").encode())
    ser.flush()
    deadline = time.time() + timeout_s
    buf = bytearray()
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        buf += b
        if b == b"\n":
            return buf.decode("ascii", errors="replace").rstrip("\r\n")
    raise TimeoutError(f"no response to {line!r} within {timeout_s}s")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--port", default=DEFAULT_PORT)
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status")
    sub.add_parser("stop")
    sub.add_parser("presets", help="list available preset names")

    sp_start = sub.add_parser("start", help="start with an inline JSON payload")
    sp_start.add_argument("payload", help='JSON string, e.g. \'{"subject_id":"S00",...}\'')

    sp_pre = sub.add_parser("start-preset", help="start with a named preset")
    sp_pre.add_argument("preset", help=f"one of: {', '.join(PRESETS.keys())}")

    sp_raw = sub.add_parser("raw", help="send an arbitrary line and print the response")
    sp_raw.add_argument("line")

    args = p.parse_args()

    if args.cmd == "presets":
        for name, payload in PRESETS.items():
            print(f"  {name}: {json.dumps(payload)}")
        return 0

    try:
        ser = connect(args.port, args.baud)
    except Exception as e:
        print(f"connect failed: {e}", file=sys.stderr)
        return 1

    if args.cmd == "status":
        line = "STATUS"
    elif args.cmd == "stop":
        line = "STOP"
    elif args.cmd == "start":
        line = "START " + args.payload
    elif args.cmd == "start-preset":
        if args.preset not in PRESETS:
            print(f"unknown preset: {args.preset}", file=sys.stderr)
            print(f"available: {', '.join(PRESETS.keys())}", file=sys.stderr)
            return 1
        line = "START " + json.dumps(PRESETS[args.preset])
    elif args.cmd == "raw":
        line = args.line
    else:
        return 2

    try:
        response = send(ser, line)
    except Exception as e:
        print(f"send failed: {e}", file=sys.stderr)
        return 1

    print(response)
    return 0 if response.startswith(("OK", "STATUS", "PONG")) else 2


if __name__ == "__main__":
    sys.exit(main())

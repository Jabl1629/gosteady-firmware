#!/usr/bin/env python3
"""
GoSteady session-file inspector (Milestone 4 dry-run ingest).

Parses the 256-byte binary header emitted by the firmware and validates
every field is either well-formed free-form ASCII or a member of the
controlled vocabulary documented in GOSTEADY_CONTEXT.md.

Two input modes:

  # Path to a .dat file on disk (once we have the USB dump path in M5):
  python tools/read_session.py path/to/<uuid>.dat

  # Base64 header as a string, scraped from the firmware's UART log line
  # `<inf> gs_session: SESSION_HEADER_B64 <base64>`:
  python tools/read_session.py --b64 "eyJ....=="

Exits 0 on valid header, 1 on any schema violation (missing magic,
unknown enum value, etc.).
"""

from __future__ import annotations

import argparse
import base64
import struct
import sys
import uuid
from dataclasses import dataclass

MAGIC = 0x53533647  # 'G','6','S','S' LE
HEADER_BYTES = 256
SAMPLE_BYTES = 28

# Struct format strings. Little-endian, no padding. Keep in lockstep with
# src/session.h — if that file's _Static_assert fails on device, this
# layout is the mirror that must change too.
HEADER_FMT = (
    "<"        # little-endian, packed
    "I"        # magic              (uint32)
    "H"        # version            (uint16)
    "H"        # header_size        (uint16)
    "16s"      # session_uuid
    "16s"      # device_serial
    "16s"      # firmware_version
    "16s"      # sensor_model
    "H"        # sample_rate_hz
    "B"        # accel_range_g
    "B"        # _pad0
    "H"        # gyro_range_dps
    "H"        # _pad1
    "q"        # session_start_utc_ms (int64)
    "q"        # session_end_utc_ms   (int64)
    "I"        # sample_count
    "H"        # battery_mv_start
    "H"        # battery_mv_end
    "H"        # flash_errors
    "H"        # _pad2
    # prewalk
    "8s"       # subject_id
    "B"        # walker_type
    "B"        # cap_type
    "16s"      # walker_model
    "B"        # mount_config
    "32s"      # course_id
    "H"        # intended_distance_ft
    "B"        # surface
    "B"        # intended_speed
    "B"        # direction
    "B"        # run_type
    "16s"      # operator_id
    # trailing reserved to bring total to 256
    "67s"      # _reserved
)
# Cross-check at import time so a format-vs-firmware drift fails loudly
# instead of misaligning.
assert struct.calcsize(HEADER_FMT) == HEADER_BYTES, (
    f"HEADER_FMT size {struct.calcsize(HEADER_FMT)} != expected {HEADER_BYTES} — "
    "C struct layout drifted from session.h"
)

SAMPLE_FMT = "<I6f"  # t_ms + 6 floats

# Controlled vocabularies — must exactly match session.h and the
# `Vocabularies` sheet of GoSteady_Capture_Annotations_v1.xlsx.
WALKER_TYPE = ["standard", "two_wheel"]
CAP_TYPE = ["tacky", "glide"]
SURFACE = [
    "polished_concrete", "low_pile_carpet", "high_pile_carpet",
    "hardwood", "tile", "linoleum", "vinyl",
    "outdoor_concrete", "outdoor_asphalt",
]
SPEED = ["slow", "normal", "fast"]
DIRECTION = ["straight", "turn_left", "turn_right", "s_curve", "pivot"]
RUN_TYPE = [
    "normal", "stumble", "pickup", "setdown", "stationary_baseline",
    "car_transport", "chair_transfer", "turn_test", "obstacle",
    "walker_type_transition", "surface_transition",
]
MOUNT_CONFIG = [
    "front_left_leg", "front_right_leg", "rear_left_leg",
    "rear_right_leg", "front_crossbar",
]


@dataclass
class Header:
    magic: int
    version: int
    header_size: int
    session_uuid: uuid.UUID
    device_serial: str
    firmware_version: str
    sensor_model: str
    sample_rate_hz: int
    accel_range_g: int
    gyro_range_dps: int
    session_start_utc_ms: int
    session_end_utc_ms: int
    sample_count: int
    battery_mv_start: int
    battery_mv_end: int
    flash_errors: int
    subject_id: str
    walker_type: str
    cap_type: str
    walker_model: str
    mount_config: str
    course_id: str
    intended_distance_ft: int
    surface: str
    intended_speed: str
    direction: str
    run_type: str
    operator_id: str


def _ascii(raw: bytes) -> str:
    """Trim trailing NULs and whitespace off a zero-padded ASCII field."""
    return raw.split(b"\0", 1)[0].decode("ascii", errors="replace").strip()


def _enum(value: int, vocab: list[str], field_name: str, errs: list[str]) -> str:
    if 0 <= value < len(vocab):
        return vocab[value]
    errs.append(f"{field_name}: value {value} not in vocabulary")
    return f"<invalid:{value}>"


def parse_header(raw: bytes) -> tuple[Header, list[str]]:
    if len(raw) < HEADER_BYTES:
        raise ValueError(f"header is {len(raw)} bytes, expected >={HEADER_BYTES}")

    fields = struct.unpack(HEADER_FMT, raw[:HEADER_BYTES])
    errs: list[str] = []

    (magic, version, hsize,
     uuid_bytes, dev_serial, fw_ver, sensor_model,
     sample_rate, accel_g, _pad0, gyro_dps, _pad1,
     start_ms, end_ms, sample_count,
     bat_start, bat_end, flash_err, _pad2,
     subject, walker_type_v, cap_type_v, walker_model,
     mount_config_v, course_id, intended_ft,
     surface_v, speed_v, direction_v, run_type_v, operator,
     _reserved) = fields

    if magic != MAGIC:
        errs.append(f"magic mismatch: got 0x{magic:08X}, expected 0x{MAGIC:08X}")
    if hsize != HEADER_BYTES:
        errs.append(f"header_size: got {hsize}, expected {HEADER_BYTES}")

    hdr = Header(
        magic=magic,
        version=version,
        header_size=hsize,
        session_uuid=uuid.UUID(bytes=uuid_bytes),
        device_serial=_ascii(dev_serial),
        firmware_version=_ascii(fw_ver),
        sensor_model=_ascii(sensor_model),
        sample_rate_hz=sample_rate,
        accel_range_g=accel_g,
        gyro_range_dps=gyro_dps,
        session_start_utc_ms=start_ms,
        session_end_utc_ms=end_ms,
        sample_count=sample_count,
        battery_mv_start=bat_start,
        battery_mv_end=bat_end,
        flash_errors=flash_err,
        subject_id=_ascii(subject),
        walker_type=_enum(walker_type_v, WALKER_TYPE, "walker_type", errs),
        cap_type=_enum(cap_type_v, CAP_TYPE, "cap_type", errs),
        walker_model=_ascii(walker_model),
        mount_config=_enum(mount_config_v, MOUNT_CONFIG, "mount_config", errs),
        course_id=_ascii(course_id),
        intended_distance_ft=intended_ft,
        surface=_enum(surface_v, SURFACE, "surface", errs),
        intended_speed=_enum(speed_v, SPEED, "intended_speed", errs),
        direction=_enum(direction_v, DIRECTION, "direction", errs),
        run_type=_enum(run_type_v, RUN_TYPE, "run_type", errs),
        operator_id=_ascii(operator),
    )
    return hdr, errs


def summarize_body(data: bytes) -> dict:
    """Return body summary including duration, derived rate, and monotonicity.

    `duration_ms` and `derived_rate_hz` are computed from min/max of all
    t_ms values, not just first/last, so they stay correct even when
    t_ms is non-monotonic. A non-monotonic stream indicates a firmware
    bug (or a leftover pre-fix session file); `t_ms_monotonic` + the
    `first_backward_jump` pointer flag it loudly so you can strip the
    contaminated prefix downstream.
    """
    if len(data) == 0:
        return {"sample_count": 0}
    if len(data) % SAMPLE_BYTES != 0:
        return {"sample_count": -1, "error": f"body length {len(data)} not a multiple of {SAMPLE_BYTES}"}

    count = len(data) // SAMPLE_BYTES
    first = struct.unpack(SAMPLE_FMT, data[:SAMPLE_BYTES])
    last = struct.unpack(SAMPLE_FMT, data[-SAMPLE_BYTES:])

    t_values = [struct.unpack_from("<I", data, i * SAMPLE_BYTES)[0] for i in range(count)]
    t_min, t_max = min(t_values), max(t_values)
    duration_ms = t_max - t_min
    derived_rate_hz = round((count - 1) / (duration_ms / 1000.0), 2) if duration_ms > 0 else 0.0

    backward_jumps = [
        (i, t_values[i - 1], t_values[i])
        for i in range(1, count)
        if t_values[i] < t_values[i - 1]
    ]

    summary = {
        "sample_count": count,
        "first_sample": {"t_ms": first[0], "ax": first[1], "ay": first[2], "az": first[3],
                         "gx": first[4], "gy": first[5], "gz": first[6]},
        "last_sample": {"t_ms": last[0], "ax": last[1], "ay": last[2], "az": last[3],
                        "gx": last[4], "gy": last[5], "gz": last[6]},
        "duration_ms": duration_ms,
        "derived_rate_hz": derived_rate_hz,
        "t_ms_monotonic": len(backward_jumps) == 0,
    }
    if backward_jumps:
        i, prev_t, cur_t = backward_jumps[0]
        summary["backward_jumps"] = len(backward_jumps)
        summary["first_backward_jump"] = (
            f"idx={i} ({100*i/count:.1f}% through body): "
            f"t[{i-1}]={prev_t} → t[{i}]={cur_t} (Δ={cur_t - prev_t})"
        )
    return summary


def pretty_print(hdr: Header, errs: list[str], body_summary: dict | None) -> None:
    print("=== FIRMWARE layer ===")
    print(f"  magic             = 0x{hdr.magic:08X}  version={hdr.version}  header_size={hdr.header_size}")
    print(f"  session_uuid      = {hdr.session_uuid}")
    print(f"  device_serial     = {hdr.device_serial!r}")
    print(f"  firmware_version  = {hdr.firmware_version!r}")
    print(f"  sensor_model      = {hdr.sensor_model!r}")
    print(f"  sample_rate_hz    = {hdr.sample_rate_hz}")
    print(f"  accel_range_g     = {hdr.accel_range_g}")
    print(f"  gyro_range_dps    = {hdr.gyro_range_dps}")
    print(f"  session_start_utc = {hdr.session_start_utc_ms} ms  (0 = RTC not yet synced, expected pre-M12)")
    print(f"  session_end_utc   = {hdr.session_end_utc_ms} ms")
    print(f"  sample_count      = {hdr.sample_count}")
    print(f"  battery_mv_start  = {hdr.battery_mv_start}  (0 = PMIC readout not yet wired)")
    print(f"  battery_mv_end    = {hdr.battery_mv_end}")
    print(f"  flash_errors      = {hdr.flash_errors}")
    print()
    print("=== PRE-WALK layer ===")
    print(f"  subject_id            = {hdr.subject_id!r}")
    print(f"  walker_type           = {hdr.walker_type}")
    print(f"  cap_type              = {hdr.cap_type}")
    print(f"  walker_model          = {hdr.walker_model!r}")
    print(f"  mount_config          = {hdr.mount_config}")
    print(f"  course_id             = {hdr.course_id!r}")
    print(f"  intended_distance_ft  = {hdr.intended_distance_ft}")
    print(f"  surface               = {hdr.surface}")
    print(f"  intended_speed        = {hdr.intended_speed}")
    print(f"  direction             = {hdr.direction}")
    print(f"  run_type              = {hdr.run_type}")
    print(f"  operator_id           = {hdr.operator_id!r}")
    if body_summary is not None:
        print()
        print("=== Body summary ===")
        for k, v in body_summary.items():
            print(f"  {k:<15} = {v}")
    print()
    if errs:
        print("=== Validation errors ===")
        for e in errs:
            print(f"  - {e}")
    else:
        print("Header validates cleanly against the v1 vocabulary.")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("path", nargs="?", help="Path to a .dat session file")
    g.add_argument("--b64", help="Base64 header string (from UART log)")
    args = p.parse_args()

    body_summary: dict | None = None
    if args.b64:
        raw = base64.b64decode(args.b64, validate=False)
    else:
        with open(args.path, "rb") as f:
            raw = f.read()
        if len(raw) >= HEADER_BYTES:
            body_summary = summarize_body(raw[HEADER_BYTES:])

    hdr, errs = parse_header(raw)
    pretty_print(hdr, errs, body_summary)
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
Join pulled session files with POST-WALK notes from capture.html.

Usage:
    python tools/ingest_capture.py \\
        --sessions raw_sessions/2026-04-22 \\
        --notes    gosteady_capture_notes_2026-04-22.json \\
        --out      capture_2026-04-22.csv

Reads every *.dat file in --sessions (pulled via tools/pull_sessions.py)
and the --notes JSON exported from capture.html's "Export capture notes"
button. Joins on `session_uuid`. Emits a CSV whose columns match the
`Captures` sheet of GoSteady_Capture_Annotations_v1.xlsx one-for-one,
with two trailing diagnostic columns (`run_idx`, `post_walk_status`)
that the operator can ignore when appending to the spreadsheet.

Exit 0 on success (including rows with warnings). Exit 1 on hard errors
(unreadable .dat file, missing required inputs).
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

# Reuse the header parser + struct format from read_session.py.
sys.path.insert(0, str(Path(__file__).parent))
from read_session import parse_header  # noqa: E402


# Column order mirrors GOSTEADY_CONTEXT.md's "Annotation Schema" section
# exactly (13 FIRMWARE + 12 PRE-WALK + 7 POST-WALK + 2 DERIVED = 34),
# plus 2 trailing diagnostic columns for ingest sanity.
SPREADSHEET_COLUMNS = [
    # FIRMWARE (13)
    "session_uuid", "device_serial", "firmware_version", "sensor_model",
    "sample_rate_hz", "accel_range_g", "gyro_range_dps",
    "session_start_utc", "session_end_utc", "sample_count",
    "battery_mv_start", "battery_mv_end", "flash_errors",
    # PRE-WALK (12)
    "subject_id", "walker_type", "cap_type", "walker_model",
    "mount_config", "course_id", "intended_distance_ft",
    "surface", "intended_speed", "direction", "run_type", "operator",
    # POST-WALK (7)
    "valid", "manual_step_count", "actual_distance_ft",
    "subjective_speed", "events", "discard_reason", "free_notes",
    # DERIVED (2)
    "duration_s", "avg_speed_ft_s",
]
DIAGNOSTIC_COLUMNS = ["run_idx", "post_walk_status"]
OUT_COLUMNS = SPREADSHEET_COLUMNS + DIAGNOSTIC_COLUMNS


def _get(note, field, default=None):
    """Pull a field from a POST-WALK note, coercing null/missing to default."""
    if not note:
        return default
    v = note.get(field, default)
    return default if v is None else v


def row_from_session(dat_path: Path, notes_dict: dict) -> tuple[dict, list[str], str]:
    """Return (row, header_errors, status) for one session file.

    status is 'complete' if a POST-WALK note was found, 'missing' otherwise.
    """
    with open(dat_path, "rb") as f:
        raw = f.read()
    hdr, errs = parse_header(raw)

    uuid = str(hdr.session_uuid)
    note = notes_dict.get(uuid)
    status = "complete" if note else "missing"

    # POST-WALK fields: apply the defaults documented in GOSTEADY_CONTEXT.md.
    # `actual_distance_ft` falls back to intended; `subjective_speed` to
    # intended. `valid` is "?" when no note exists — loud signal that
    # the operator must fix up manually before spreadsheet append.
    actual_dist = _get(note, "actual_distance_ft", hdr.intended_distance_ft)
    subj_speed = _get(note, "subjective_speed", hdr.intended_speed)
    valid_v = _get(note, "valid", "?")

    # DERIVED: session_start/end_utc_ms are 0 pre-M12, so we can't use
    # them. Derive duration from sample_count / sample_rate_hz (both
    # come from the firmware-stamped header). Close enough for v1.
    if hdr.sample_count and hdr.sample_rate_hz:
        duration_s = round(hdr.sample_count / hdr.sample_rate_hz, 3)
    else:
        duration_s = 0.0
    try:
        ad = float(actual_dist) if actual_dist not in ("", None) else 0.0
    except (TypeError, ValueError):
        ad = 0.0
    avg_speed = round(ad / duration_s, 3) if duration_s > 0 else 0.0

    row = {
        # FIRMWARE
        "session_uuid": uuid,
        "device_serial": hdr.device_serial,
        "firmware_version": hdr.firmware_version,
        "sensor_model": hdr.sensor_model,
        "sample_rate_hz": hdr.sample_rate_hz,
        "accel_range_g": hdr.accel_range_g,
        "gyro_range_dps": hdr.gyro_range_dps,
        "session_start_utc": hdr.session_start_utc_ms,
        "session_end_utc": hdr.session_end_utc_ms,
        "sample_count": hdr.sample_count,
        "battery_mv_start": hdr.battery_mv_start,
        "battery_mv_end": hdr.battery_mv_end,
        "flash_errors": hdr.flash_errors,
        # PRE-WALK
        "subject_id": hdr.subject_id,
        "walker_type": hdr.walker_type,
        "cap_type": hdr.cap_type,
        "walker_model": hdr.walker_model,
        "mount_config": hdr.mount_config,
        "course_id": hdr.course_id,
        "intended_distance_ft": hdr.intended_distance_ft,
        "surface": hdr.surface,
        "intended_speed": hdr.intended_speed,
        "direction": hdr.direction,
        "run_type": hdr.run_type,
        "operator": hdr.operator_id,
        # POST-WALK
        "valid": valid_v,
        "manual_step_count": _get(note, "manual_step_count", ""),
        "actual_distance_ft": actual_dist,
        "subjective_speed": subj_speed,
        "events": _get(note, "events", ""),
        "discard_reason": _get(note, "discard_reason", ""),
        "free_notes": _get(note, "free_notes", ""),
        # DERIVED
        "duration_s": duration_s,
        "avg_speed_ft_s": avg_speed,
        # Diagnostics
        "run_idx": _get(note, "run_idx", ""),
        "post_walk_status": status,
    }
    return row, errs, status


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--sessions", required=True,
                   help="directory containing .dat files (from pull_sessions.py)")
    p.add_argument("--notes", required=True,
                   help="capture notes JSON exported from capture.html")
    p.add_argument("--out", required=True, help="output CSV path")
    args = p.parse_args()

    sessions_dir = Path(args.sessions)
    if not sessions_dir.is_dir():
        print(f"error: --sessions not a directory: {sessions_dir}", file=sys.stderr)
        return 1

    notes_path = Path(args.notes)
    if not notes_path.is_file():
        print(f"error: --notes file not found: {notes_path}", file=sys.stderr)
        return 1
    with open(notes_path) as f:
        payload = json.load(f)
    if payload.get("schema_version") != 1:
        print(f"warn: notes file schema_version is {payload.get('schema_version')!r} — expected 1",
              file=sys.stderr)
    notes_dict = payload.get("notes", {})

    dat_files = sorted(sessions_dir.glob("*.dat"))
    if not dat_files:
        print(f"error: no .dat files in {sessions_dir}", file=sys.stderr)
        return 1

    rows: list[dict] = []
    seen_uuids: set[str] = set()
    n_complete = n_missing = 0
    hard_errors: list[tuple[str, str]] = []

    for dat in dat_files:
        try:
            row, errs, status = row_from_session(dat, notes_dict)
            if errs:
                print(f"  WARN: {dat.name}: header errors: {', '.join(errs)}",
                      file=sys.stderr)
            if status == "complete":
                n_complete += 1
            else:
                n_missing += 1
                print(f"  WARN: {dat.name}: no POST-WALK note (uuid={row['session_uuid']})",
                      file=sys.stderr)
            rows.append(row)
            seen_uuids.add(row["session_uuid"])
        except Exception as e:
            hard_errors.append((dat.name, str(e)))
            print(f"  ERROR: {dat.name}: {e}", file=sys.stderr)

    orphan_notes = [u for u in notes_dict if u not in seen_uuids]
    for u in orphan_notes:
        n = notes_dict[u]
        print(f"  WARN: orphan note: uuid={u} run_idx={n.get('run_idx')} "
              "(no matching .dat on disk)", file=sys.stderr)

    # Sort by run_idx for readability; orphan/unknown run_idx sinks to the bottom.
    def _key(r: dict):
        try:
            return (int(r.get("run_idx") or 999_999), r["session_uuid"])
        except (TypeError, ValueError):
            return (999_999, r["session_uuid"])
    rows.sort(key=_key)

    out_path = Path(args.out)
    with open(out_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=OUT_COLUMNS, extrasaction="ignore")
        writer.writeheader()
        for r in rows:
            writer.writerow(r)

    print()
    print(f"wrote {len(rows)} row(s) to {out_path}")
    print(f"  POST-WALK complete: {n_complete}")
    print(f"  POST-WALK missing:  {n_missing}")
    if orphan_notes:
        print(f"  orphan notes:       {len(orphan_notes)}")
    if hard_errors:
        print(f"  hard errors:        {len(hard_errors)}")
        return 1
    print()
    print("Next: append the first 34 columns of this CSV to the Captures sheet of")
    print("GoSteady_Capture_Annotations_v1.xlsx. (The trailing run_idx and")
    print("post_walk_status columns are diagnostic — drop them before append.)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

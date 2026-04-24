"""Load a GoSteady capture-day dataset into per-run Python objects.

The loader takes one `raw_sessions/<date>/` directory and returns a list
of `Run` objects ordered by `run_idx` from the capture CSV. Each `Run`
bundles:

- The parsed firmware header (FIRMWARE layer of the v1 schema)
- The full PRE-WALK + POST-WALK annotation row from `capture_<date>.csv`
- The IMU body as a structured numpy array

Units in `Run.imu` are exactly what the firmware wrote: accel in m/s²,
gyro in rad/s. Convenience properties `accel_g` and `gyro_dps` are
provided for comparability with the old Python algorithm which operated
in g / deg·s⁻¹. The canonical streaming pipeline should stay in SI so
the M10 C port doesn't need unit conversion in the sample-rate hot path.

The header parser is imported from `tools/read_session.py` (not
duplicated) so this stays locked to `src/session.h` via that file's
`_Static_assert`-mirrored format string.
"""

from __future__ import annotations

import csv
import json
import struct
import sys
import warnings
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Iterator

import numpy as np

# Pull in the canonical header parser. Add the repo root's `tools/`
# directory to sys.path on first import so this works regardless of how
# Python is invoked (module, script, notebook).
_REPO_ROOT = Path(__file__).resolve().parent.parent
if str(_REPO_ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT / "tools"))

import read_session as _rs  # noqa: E402  (late import by design)

HEADER_BYTES = _rs.HEADER_BYTES
SAMPLE_BYTES = _rs.SAMPLE_BYTES
SAMPLE_FMT = _rs.SAMPLE_FMT

# Numpy dtype mirroring the 28-byte packed sample record from session.h.
# `<I6f` = little-endian uint32 + 6 float32s. Keeping this in lockstep
# with SAMPLE_FMT lets us fromfile-read the body in one shot.
SAMPLE_DTYPE = np.dtype(
    [
        ("t_ms", "<u4"),
        ("ax", "<f4"),
        ("ay", "<f4"),
        ("az", "<f4"),
        ("gx", "<f4"),
        ("gy", "<f4"),
        ("gz", "<f4"),
    ]
)
assert SAMPLE_DTYPE.itemsize == SAMPLE_BYTES, (
    f"SAMPLE_DTYPE size {SAMPLE_DTYPE.itemsize} != {SAMPLE_BYTES} — "
    "drift from session.h sample format"
)

_G = 9.80665  # exact, per CODATA. Used only for accel_g convenience view.
_DEG_PER_RAD = 180.0 / np.pi


# ---------------------------------------------------------------------
# Run object
# ---------------------------------------------------------------------


@dataclass
class Run:
    """A single session — IMU body plus all annotation layers.

    Training code should never mutate `imu`; use views/copies downstream.
    """

    run_idx: int
    session_uuid: str
    annotations: dict[str, Any]   # full CSV row, verbatim
    header: _rs.Header             # parsed FIRMWARE layer from the .dat
    imu: np.ndarray                # structured array of shape (N,) dtype=SAMPLE_DTYPE
    dat_path: Path | None          # None if no .dat could be located
    notes: dict[str, Any] = field(default_factory=dict)  # POST-WALK JSON entry

    # ---- Annotation shortcuts (typed) -------------------------------

    @property
    def course_id(self) -> str:
        return str(self.annotations["course_id"])

    @property
    def run_type(self) -> str:
        return str(self.annotations["run_type"])

    @property
    def direction(self) -> str:
        return str(self.annotations["direction"])

    @property
    def intended_speed(self) -> str:
        return str(self.annotations["intended_speed"])

    @property
    def actual_distance_ft(self) -> float:
        """Ground-truth distance per GOSTEADY_CONTEXT.md §M9 discussion."""
        v = self.annotations.get("actual_distance_ft")
        return float(v) if v not in (None, "") else float("nan")

    @property
    def intended_distance_ft(self) -> float:
        v = self.annotations.get("intended_distance_ft")
        return float(v) if v not in (None, "") else float("nan")

    @property
    def manual_step_count(self) -> float:
        """NaN when the operator didn't record a step count for this run."""
        v = self.annotations.get("manual_step_count")
        if v in (None, ""):
            return float("nan")
        return float(v)

    @property
    def valid(self) -> bool:
        return str(self.annotations.get("valid", "N")).upper() == "Y"

    @property
    def is_walk(self) -> bool:
        """True for runs the distance estimator should actually try to get right.

        Stationary baseline + stumble are robustness gates (GOSTEADY_CONTEXT.md
        §M9 answer 3), not training data. Everything else is fair game.
        """
        return self.run_type not in ("stationary_baseline", "stumble")

    # ---- IMU views --------------------------------------------------

    @property
    def n_samples(self) -> int:
        return int(self.imu.shape[0])

    @property
    def t_s(self) -> np.ndarray:
        """Sample timestamps in seconds, float64. `imu['t_ms']` is the raw view."""
        return self.imu["t_ms"].astype(np.float64) / 1000.0

    @property
    def derived_rate_hz(self) -> float:
        """Actual sample rate, computed the same way read_session.py does
        (min/max of t_ms, robust to non-monotonic streams)."""
        if self.n_samples < 2:
            return 0.0
        t = self.imu["t_ms"]
        dur_s = (int(t.max()) - int(t.min())) / 1000.0
        return (self.n_samples - 1) / dur_s if dur_s > 0 else 0.0

    @property
    def t_ms_monotonic(self) -> bool:
        t = self.imu["t_ms"]
        return bool(np.all(np.diff(t.astype(np.int64)) >= 0))

    @property
    def accel_ms2(self) -> np.ndarray:
        """(N, 3) float32 accel in m/s² — native firmware units."""
        return np.stack(
            [self.imu["ax"], self.imu["ay"], self.imu["az"]], axis=1
        )

    @property
    def gyro_rads(self) -> np.ndarray:
        """(N, 3) float32 gyro in rad/s — native firmware units."""
        return np.stack(
            [self.imu["gx"], self.imu["gy"], self.imu["gz"]], axis=1
        )

    @property
    def accel_g(self) -> np.ndarray:
        """(N, 3) accel in g. Convenience for comparing against the old
        Python algo which worked in g."""
        return self.accel_ms2 / _G

    @property
    def gyro_dps(self) -> np.ndarray:
        """(N, 3) gyro in deg/s."""
        return self.gyro_rads * _DEG_PER_RAD

    @property
    def accel_mag_ms2(self) -> np.ndarray:
        """(N,) |a|. The primary step-detection channel."""
        a = self.accel_ms2
        return np.sqrt((a * a).sum(axis=1))

    @property
    def accel_mag_g(self) -> np.ndarray:
        return self.accel_mag_ms2 / _G


# ---------------------------------------------------------------------
# File-level parsers
# ---------------------------------------------------------------------


def parse_dat_file(path: Path) -> tuple[_rs.Header, np.ndarray, list[str]]:
    """Read one .dat file. Returns (header, imu_array, header_validation_errors).

    Does NOT raise on header-validation errors — they come back as a list
    so a partially-corrupt session is still usable for body-only work.
    The file IS required to have a full 256-byte header followed by an
    integral number of 28-byte sample records; anything else raises.
    """
    raw = path.read_bytes()
    if len(raw) < HEADER_BYTES:
        raise ValueError(f"{path.name}: {len(raw)} bytes < {HEADER_BYTES} header")

    header, errs = _rs.parse_header(raw[:HEADER_BYTES])
    body = raw[HEADER_BYTES:]
    if len(body) % SAMPLE_BYTES != 0:
        raise ValueError(
            f"{path.name}: body length {len(body)} not a multiple of "
            f"{SAMPLE_BYTES} (corrupt or truncated)"
        )
    imu = np.frombuffer(body, dtype=SAMPLE_DTYPE)
    return header, imu, errs


def _read_capture_csv(path: Path) -> list[dict[str, Any]]:
    """Read capture_<date>.csv. Returns raw string-valued rows ordered by
    `run_idx` ascending; downstream typing happens in `Run` properties."""
    with path.open(newline="") as f:
        rows = list(csv.DictReader(f))
    try:
        rows.sort(key=lambda r: int(r.get("run_idx") or 0))
    except (TypeError, ValueError):
        # run_idx missing/malformed — leave order as-is.
        pass
    return rows


def _read_post_walk_notes(path: Path | None) -> dict[str, dict[str, Any]]:
    """Load `gosteady_capture_notes_<date>.json` keyed by session_uuid.

    The M7 sidecar schema is `{ "schema_version": 1, "entries": {<uuid>: {...}} }`
    but if the file is missing we degrade gracefully to `{}`. POST-WALK
    data is also already denormalized into the capture CSV by
    `tools/ingest_capture.py`, so this is mostly for access to any
    sidecar fields the CSV doesn't carry (e.g. per-entry timestamps).
    """
    if path is None or not path.exists():
        return {}
    with path.open() as f:
        doc = json.load(f)
    entries = doc.get("entries") or {}
    if not isinstance(entries, dict):
        return {}
    return entries


# ---------------------------------------------------------------------
# Dataset loader
# ---------------------------------------------------------------------


def _find_capture_csv(dir: Path) -> Path:
    csvs = sorted(dir.glob("capture_*.csv"))
    if not csvs:
        raise FileNotFoundError(f"no capture_*.csv under {dir}")
    if len(csvs) > 1:
        warnings.warn(f"{len(csvs)} capture CSVs under {dir}; using {csvs[-1].name}")
    return csvs[-1]


def _find_notes_json(dir: Path) -> Path | None:
    jsons = sorted(dir.glob("gosteady_capture_notes_*.json"))
    return jsons[-1] if jsons else None


def load_capture_day(dir: str | Path) -> list[Run]:
    """Load one `raw_sessions/<date>/` directory into a list of Runs.

    The capture CSV is the source of truth for which runs exist and
    their annotation fields. A row missing its matching .dat file emits
    a warning and is skipped (so callers can freely filter on `.is_walk`
    etc. without tripping on missing bodies).
    """
    d = Path(dir).expanduser().resolve()
    if not d.is_dir():
        raise NotADirectoryError(d)

    csv_path = _find_capture_csv(d)
    notes_by_uuid = _read_post_walk_notes(_find_notes_json(d))

    runs: list[Run] = []
    for row in _read_capture_csv(csv_path):
        uuid_str = row.get("session_uuid") or ""
        if not uuid_str:
            warnings.warn(f"row with no session_uuid in {csv_path.name}; skipping")
            continue

        dat_path = d / f"{uuid_str}.dat"
        if not dat_path.exists():
            warnings.warn(f"no .dat for session_uuid={uuid_str}; skipping row")
            continue

        header, imu, header_errs = parse_dat_file(dat_path)
        if header_errs:
            warnings.warn(
                f"{dat_path.name}: header validation errors: {header_errs}"
            )

        try:
            run_idx = int(row.get("run_idx") or 0)
        except ValueError:
            run_idx = 0

        runs.append(
            Run(
                run_idx=run_idx,
                session_uuid=uuid_str,
                annotations=dict(row),
                header=header,
                imu=imu,
                dat_path=dat_path,
                notes=dict(notes_by_uuid.get(uuid_str, {})),
            )
        )
    return runs


def iter_walks(runs: Iterable[Run]) -> Iterator[Run]:
    """Convenience: yields only runs where `.is_walk` is True and
    `actual_distance_ft` is a finite positive number. Use this to build
    the training / LOO set; filter separately for robustness-gate runs."""
    for r in runs:
        d = r.actual_distance_ft
        if r.is_walk and d > 0 and np.isfinite(d):
            yield r

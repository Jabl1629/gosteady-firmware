"""Emit reference vectors for the M10 C-port regression suite.

For each chosen `.dat` session, run the locked V1 pipeline (filter →
motion gate → step detector → roughness → surface classifier → stride
regression) with per-sample hooks. Capture every intermediate signal
and dump as a packed binary + JSON sidecar. The C port's unit tests
must reproduce these outputs bit-close (modulo float32 vs float64
roundoff — the binary stores float32, and the C code is float32, so
the comparison budget is tight).

Output files (per vector):
    src/algo/test_vectors/<name>.bin   — packed float32 + uint8 arrays
    src/algo/test_vectors/<name>.json  — metadata, scalars, layout map

Binary layout (little-endian; matches Cortex-M and host x86):

    +0   u8[8]   magic = "GS_REFVT"
    +8   u32     version = 1
    +12  u32     n_samples
    +16  u32     n_peaks
    +20  u32     surface_class (0=INDOOR, 1=OUTDOOR)
    +24  f32     fs_hz
    +28  f32     roughness_R
    +32  f32     distance_ft
    +36  f32     motion_duration_s
    +40  f32     motion_fraction
    +44  f32     total_duration_s
                                                             (header = 48 B)
    +48           f32 mag_g[n_samples]            (input |a| in g)
    +48 + 4N      f32 mag_hp[n_samples]           (after HP filter)
    +48 + 8N      f32 mag_lp[n_samples]           (after LP filter)
    +48 + 12N     u8  motion_mask[n_samples]      (gate output, packed bool)
    +48 + 13N     (pad to 4-byte align)
                                                  (peaks block follows)
                  Peak peaks[n_peaks]             each 20 B:
                      u32 sample_idx
                      f32 time_s
                      f32 amplitude_g
                      f32 duration_s
                      f32 energy_g2s

C-side loader: ~50 lines; see write_test_vector() for the exact byte
positions to mirror. The Python verify() at the bottom of this file
re-reads the binary it just wrote and checks every field — copy that
logic to the C unit test.

Run from repo root:
    algo/venv/bin/python3 -m algo.export_reference_vectors
"""

from __future__ import annotations

import datetime as _dt
import json
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from . import export_c_header as _params
from .data_loader import Run, load_capture_day
from .filters import butterworth_hp, butterworth_lp
from .motion_gate import MotionGate
from .roughness import inter_peak_rms_g
from .step_detector import Peak, StepDetector


MAGIC = b"GS_REFVT"
VERSION = 1
HEADER_BYTES = 48
PEAK_BYTES = 20  # u32 + 4×f32


# =====================================================================
# Vector specifications
# =====================================================================


@dataclass(frozen=True)
class VectorSpec:
    """One reference vector to emit."""
    name: str                # filesystem-friendly short name
    dataset_dir: str         # repo-relative
    session_uuid: str
    note: str                # human-readable description (goes into JSON)


VECTORS: list[VectorSpec] = [
    VectorSpec(
        name="indoor_run05_walk_20ft",
        dataset_dir="raw_sessions/2026-04-23",
        session_uuid="0778cb85-177d-4e82-94cc-62a8cffb6be2",
        note=("Indoor polished concrete, run 5, normal speed, 20 ft straight. "
              "Canonical walk vector — passes through the indoor surface "
              "classifier branch."),
    ),
    VectorSpec(
        name="indoor_run09_stationary_30s",
        dataset_dir="raw_sessions/2026-04-23",
        session_uuid="ca82711e-12ab-4024-b4a6-e79eb3233887",
        note=("Indoor polished concrete, run 9, stationary baseline, 30 s "
              "parked. Robustness vector — distance must be < 1 ft."),
    ),
    VectorSpec(
        name="outdoor_run13_walk_20ft",
        dataset_dir="raw_sessions/2026-04-25",
        session_uuid="b1e7ef3b-e7fd-4afc-9567-9fe5e9a14740",
        note=("Outdoor concrete sidewalk, run 13, normal speed, 20 ft straight. "
              "Cross-surface vector — passes through the outdoor surface "
              "classifier branch."),
    ),
]


# =====================================================================
# Run the V1 pipeline with hooks
# =====================================================================


@dataclass
class RefVector:
    """Captured pipeline outputs for one session."""
    run: Run
    fs_hz: float
    mag_g: np.ndarray            # float32 (n,)
    mag_hp: np.ndarray           # float32 (n,)
    mag_lp: np.ndarray           # float32 (n,)
    motion_mask: np.ndarray      # uint8 (n,)
    peaks: list[Peak]
    roughness_R: float
    surface_class: int           # 0=indoor, 1=outdoor
    surface_name: str            # "indoor" / "outdoor"
    indoor_c0: float
    indoor_c1: float
    outdoor_c0: float
    outdoor_c1: float
    distance_ft: float
    motion_duration_s: float
    motion_fraction: float
    total_duration_s: float


def _process(run: Run,
              indoor_c0: float, indoor_c1: float,
              outdoor_c0: float, outdoor_c1: float) -> RefVector:
    """Run the locked V1 pipeline with sample-level capture.

    Mirrors `algo.export_c_header._process_run` exactly — same constants,
    same order of operations — but additionally captures `mag_g`,
    `mag_hp`, and the surface decision so we can emit a complete fixture.
    """
    fs = run.derived_rate_hz or _params.FS_HZ
    hp = butterworth_hp(_params.HP_CUTOFF_HZ, fs=fs, order=_params.FILTER_ORDER)
    lp = butterworth_lp(_params.LP_CUTOFF_HZ, fs=fs, order=_params.FILTER_ORDER)
    det = StepDetector(
        fs=fs,
        enter_threshold_g=_params.PEAK_ENTER_G,
        exit_threshold_g=_params.PEAK_EXIT_G,
        min_gap_s=_params.PEAK_MIN_GAP_S,
        max_active_s=_params.PEAK_MAX_ACTIVE_S,
    )
    gate = MotionGate(
        fs=fs,
        window_samples=int(round(_params.GATE_WINDOW_S * fs)),
        enter_threshold=_params.GATE_ENTER_G,
        exit_threshold=_params.GATE_EXIT_G,
        exit_hold_samples=int(round(_params.GATE_EXIT_HOLD_S * fs)),
    )

    mag_g_f64 = run.accel_mag_g.astype(np.float64)
    n = len(mag_g_f64)
    mag_hp = np.empty(n, dtype=np.float64)
    mag_lp = np.empty(n, dtype=np.float64)
    motion_mask = np.zeros(n, dtype=bool)

    hp.init_steady(float(mag_g_f64[0]) - 1.0)
    peaks: list[Peak] = []
    for i in range(n):
        v_hp = hp.step(float(mag_g_f64[i]) - 1.0)
        mag_hp[i] = v_hp
        v_lp = lp.step(v_hp)
        mag_lp[i] = v_lp
        motion_mask[i] = gate.step(v_hp)
        p = det.step(v_lp)
        if p is not None:
            peaks.append(p)

    R = inter_peak_rms_g(
        mag_lp,
        [p.sample_idx for p in peaks],
        fs,
        half_window_s=_params.ROUGH_HALF_WINDOW_S,
        motion_mask=motion_mask,
    )

    # Surface classification — hard threshold on R (NaN R defaults to
    # indoor; this matches what the firmware should do when there's no
    # walking motion to compute R from, and harmlessly falls through
    # because the stride sum will be ~0).
    if not np.isfinite(R):
        surface_class = 0
        surface_name = "indoor"
    elif R < _params.R_THRESHOLD:
        surface_class = 0
        surface_name = "indoor"
    else:
        surface_class = 1
        surface_name = "outdoor"

    if surface_class == 0:
        c0, c1 = indoor_c0, indoor_c1
    else:
        c0, c1 = outdoor_c0, outdoor_c1

    if peaks:
        sum_amp = sum(p.amplitude_g for p in peaks)
        distance_ft = max(c0 * len(peaks) + c1 * sum_amp, 0.0)
    else:
        distance_ft = 0.0

    # Cast arrays to float32 / uint8 for the binary fixture. Keeping
    # the Python computation in float64 and casting at the boundary is
    # the same pattern the data loader uses.
    return RefVector(
        run=run,
        fs_hz=float(fs),
        mag_g=mag_g_f64.astype(np.float32),
        mag_hp=mag_hp.astype(np.float32),
        mag_lp=mag_lp.astype(np.float32),
        motion_mask=motion_mask.astype(np.uint8),
        peaks=peaks,
        roughness_R=float(R) if np.isfinite(R) else float("nan"),
        surface_class=surface_class,
        surface_name=surface_name,
        indoor_c0=indoor_c0,
        indoor_c1=indoor_c1,
        outdoor_c0=outdoor_c0,
        outdoor_c1=outdoor_c1,
        distance_ft=float(distance_ft),
        motion_duration_s=float(gate.motion_duration_s),
        motion_fraction=float(gate.motion_fraction),
        total_duration_s=float(gate.total_duration_s),
    )


# =====================================================================
# Binary writer
# =====================================================================


def write_test_vector(rv: RefVector, out_bin: Path, out_json: Path,
                       *, spec: VectorSpec, git_sha: str, gen_utc: str) -> None:
    """Write the binary + JSON sidecar atomically."""
    n = len(rv.mag_g)
    n_peaks = len(rv.peaks)

    # ---- Header (48 B) ----
    # Use struct '<' for little-endian; padding is implicit because we
    # specify exact field widths.
    header = struct.pack(
        "<8sIIII6f",
        MAGIC,
        VERSION,
        n,
        n_peaks,
        rv.surface_class,
        rv.fs_hz,
        rv.roughness_R if np.isfinite(rv.roughness_R) else float("nan"),
        rv.distance_ft,
        rv.motion_duration_s,
        rv.motion_fraction,
        rv.total_duration_s,
    )
    assert len(header) == HEADER_BYTES, (
        f"header packed to {len(header)} bytes; expected {HEADER_BYTES}"
    )

    # ---- Per-sample arrays ----
    mag_g_bytes = rv.mag_g.tobytes(order="C")
    mag_hp_bytes = rv.mag_hp.tobytes(order="C")
    mag_lp_bytes = rv.mag_lp.tobytes(order="C")
    motion_mask_bytes = rv.motion_mask.tobytes(order="C")

    samples_block_len = 13 * n  # 3×f32 + 1×u8
    pad_to_align = (-samples_block_len) % 4
    pad_bytes = b"\x00" * pad_to_align

    # ---- Peaks block ----
    peak_chunks: list[bytes] = []
    for p in rv.peaks:
        peak_chunks.append(struct.pack(
            "<I4f",
            int(p.sample_idx),
            float(p.time_s),
            float(p.amplitude_g),
            float(p.duration_s),
            float(p.energy_g2s),
        ))
    peaks_bytes = b"".join(peak_chunks)
    assert len(peaks_bytes) == n_peaks * PEAK_BYTES

    # ---- Assemble + write ----
    blob = (header
            + mag_g_bytes
            + mag_hp_bytes
            + mag_lp_bytes
            + motion_mask_bytes
            + pad_bytes
            + peaks_bytes)
    out_bin.write_bytes(blob)

    # ---- Sidecar JSON ----
    sidecar = {
        "magic": MAGIC.decode("ascii"),
        "version": VERSION,
        "name": spec.name,
        "note": spec.note,
        "source_dat": str(rv.run.dat_path.relative_to(_params.Path(__file__)
                                                     .resolve().parent.parent)
                          ) if rv.run.dat_path else None,
        "session_uuid": rv.run.session_uuid,
        "annotations_subset": {
            "course_id": rv.run.annotations.get("course_id"),
            "intended_distance_ft": rv.run.annotations.get("intended_distance_ft"),
            "actual_distance_ft": rv.run.annotations.get("actual_distance_ft"),
            "intended_speed": rv.run.annotations.get("intended_speed"),
            "direction": rv.run.annotations.get("direction"),
            "run_type": rv.run.annotations.get("run_type"),
            "surface": rv.run.annotations.get("surface"),
        },
        "n_samples": n,
        "n_peaks": n_peaks,
        "fs_hz": rv.fs_hz,
        "surface_class": rv.surface_class,
        "surface_name": rv.surface_name,
        "session_outputs": {
            "step_count": n_peaks,
            "motion_duration_s": rv.motion_duration_s,
            "motion_fraction": rv.motion_fraction,
            "total_duration_s": rv.total_duration_s,
            "roughness_R": rv.roughness_R if np.isfinite(rv.roughness_R) else None,
            "distance_ft": rv.distance_ft,
        },
        "coeffs_in_use": {
            "indoor": [rv.indoor_c0, rv.indoor_c1],
            "outdoor": [rv.outdoor_c0, rv.outdoor_c1],
            "selected_surface": rv.surface_name,
        },
        "binary_layout": {
            "header_bytes": HEADER_BYTES,
            "samples_block_offset": HEADER_BYTES,
            "mag_g_offset": HEADER_BYTES,
            "mag_hp_offset": HEADER_BYTES + 4 * n,
            "mag_lp_offset": HEADER_BYTES + 8 * n,
            "motion_mask_offset": HEADER_BYTES + 12 * n,
            "peaks_offset": HEADER_BYTES + 13 * n + pad_to_align,
            "peak_record_bytes": PEAK_BYTES,
            "total_bytes": len(blob),
        },
        "algo_version": _params.ALGO_VERSION_STR,
        "algo_params_git_sha": git_sha,
        "generated_utc": gen_utc,
    }
    out_json.write_text(json.dumps(sidecar, indent=2) + "\n")


# =====================================================================
# Self-verification (model for the C-side loader)
# =====================================================================


def verify(out_bin: Path, out_json: Path, rv: RefVector) -> None:
    """Re-read the binary + sidecar and check every field round-trips.

    The C unit test should mirror this loader function — same byte
    offsets, same field widths, same comparisons.
    """
    blob = out_bin.read_bytes()
    sidecar = json.loads(out_json.read_text())
    n = sidecar["n_samples"]
    n_peaks = sidecar["n_peaks"]

    # Header
    (magic, version, n_h, np_h, surf, fs,
     R, dist, mdur, mfrac, tdur) = struct.unpack_from("<8sIIII6f", blob, 0)
    assert magic == MAGIC, f"magic mismatch: {magic!r}"
    assert version == VERSION, f"version: {version}"
    assert n_h == n and np_h == n_peaks
    assert surf == rv.surface_class
    assert abs(fs - rv.fs_hz) < 1e-3, f"fs: {fs} vs {rv.fs_hz}"
    assert abs(dist - rv.distance_ft) < 1e-3, f"dist: {dist} vs {rv.distance_ft}"

    # Per-sample arrays
    layout = sidecar["binary_layout"]
    mag_g_back = np.frombuffer(blob, dtype=np.float32,
                                count=n, offset=layout["mag_g_offset"])
    mag_hp_back = np.frombuffer(blob, dtype=np.float32,
                                 count=n, offset=layout["mag_hp_offset"])
    mag_lp_back = np.frombuffer(blob, dtype=np.float32,
                                 count=n, offset=layout["mag_lp_offset"])
    mm_back = np.frombuffer(blob, dtype=np.uint8,
                             count=n, offset=layout["motion_mask_offset"])
    assert np.array_equal(mag_g_back, rv.mag_g)
    assert np.array_equal(mag_hp_back, rv.mag_hp)
    assert np.array_equal(mag_lp_back, rv.mag_lp)
    assert np.array_equal(mm_back, rv.motion_mask)

    # Peaks
    peaks_back = []
    for i in range(n_peaks):
        off = layout["peaks_offset"] + i * PEAK_BYTES
        sample_idx, t, amp, dur, energy = struct.unpack_from("<I4f", blob, off)
        peaks_back.append((sample_idx, t, amp, dur, energy))
    assert len(peaks_back) == len(rv.peaks)
    for i, (orig, back) in enumerate(zip(rv.peaks, peaks_back)):
        assert orig.sample_idx == back[0], f"peak[{i}].sample_idx mismatch"
        assert abs(orig.time_s - back[1]) < 1e-3
        assert abs(orig.amplitude_g - back[2]) < 1e-5
        assert abs(orig.duration_s - back[3]) < 1e-3
        assert abs(orig.energy_g2s - back[4]) < 1e-5


# =====================================================================
# Driver
# =====================================================================


def _git_sha(repo_root: Path) -> str:
    try:
        out = subprocess.run(
            ["git", "-C", str(repo_root), "rev-parse", "--short=12", "HEAD"],
            capture_output=True, text=True, check=True,
        )
        sha = out.stdout.strip()
        dirty = subprocess.run(
            ["git", "-C", str(repo_root), "status", "--porcelain"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
        return sha + (" (dirty)" if dirty else "")
    except Exception:
        return "unknown"


def _fit_full_surface_coeffs(walks: list[Run]) -> tuple[float, float]:
    """Mirror of `export_c_header._fit_surface_coeffs` — same recipe so
    the reference vectors are computed against the exact coefficients
    the C header carries."""
    rows = []
    y = []
    for r in walks:
        # Re-run the pipeline with NO surface info to extract peaks
        # only — coefficient values from this fit are what get baked
        # into the header AND used for prediction here.
        fs = r.derived_rate_hz or _params.FS_HZ
        hp = butterworth_hp(_params.HP_CUTOFF_HZ, fs=fs, order=_params.FILTER_ORDER)
        lp = butterworth_lp(_params.LP_CUTOFF_HZ, fs=fs, order=_params.FILTER_ORDER)
        det = StepDetector(
            fs=fs,
            enter_threshold_g=_params.PEAK_ENTER_G,
            exit_threshold_g=_params.PEAK_EXIT_G,
            min_gap_s=_params.PEAK_MIN_GAP_S,
            max_active_s=_params.PEAK_MAX_ACTIVE_S,
        )
        mag_g = r.accel_mag_g
        hp.init_steady(float(mag_g[0]) - 1.0)
        peaks: list[Peak] = []
        for x in mag_g:
            v_hp = hp.step(float(x) - 1.0)
            v_lp = lp.step(v_hp)
            p = det.step(v_lp)
            if p is not None:
                peaks.append(p)
        sum_amp = sum(p.amplitude_g for p in peaks)
        rows.append([float(len(peaks)), float(sum_amp)])
        y.append(r.actual_distance_ft)
    X = np.array(rows, dtype=np.float64)
    y = np.array(y, dtype=np.float64)
    A = X.T @ X + 1e-3 * np.eye(2)
    c = np.linalg.solve(A, X.T @ y)
    return float(c[0]), float(c[1])


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    out_dir = repo_root / "src" / "algo" / "test_vectors"
    out_dir.mkdir(parents=True, exist_ok=True)

    git_sha = _git_sha(repo_root)
    gen_utc = _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds")

    # Fit per-surface coefficients on full available data — must match
    # what export_c_header writes to the C header.
    print("Fitting per-surface stride coefficients...")
    indoor_runs = load_capture_day(repo_root / _params.INDOOR_DATASET)
    indoor_walks = [r for r in indoor_runs if r.is_walk
                    and r.actual_distance_ft > 0
                    and np.isfinite(r.actual_distance_ft)
                    and r.valid]
    outdoor_runs = load_capture_day(repo_root / _params.OUTDOOR_DATASET)
    outdoor_walks = [r for r in outdoor_runs if r.is_walk
                     and r.actual_distance_ft > 0
                     and np.isfinite(r.actual_distance_ft)
                     and r.valid]
    indoor_c0, indoor_c1 = _fit_full_surface_coeffs(indoor_walks)
    outdoor_c0, outdoor_c1 = _fit_full_surface_coeffs(outdoor_walks)
    print(f"  indoor : stride_ft = {indoor_c0:+.4f} + {indoor_c1:+.4f}·amp_g  "
          f"(n={len(indoor_walks)})")
    print(f"  outdoor: stride_ft = {outdoor_c0:+.4f} + {outdoor_c1:+.4f}·amp_g  "
          f"(n={len(outdoor_walks)})")
    print()

    # Build a uuid → Run lookup across both capture days
    all_runs: dict[tuple[str, str], Run] = {}
    for r in indoor_runs:
        all_runs[(_params.INDOOR_DATASET, r.session_uuid)] = r
    for r in outdoor_runs:
        all_runs[(_params.OUTDOOR_DATASET, r.session_uuid)] = r

    for spec in VECTORS:
        run = all_runs.get((spec.dataset_dir, spec.session_uuid))
        if run is None:
            print(f"  SKIP {spec.name}: uuid {spec.session_uuid} not in "
                  f"{spec.dataset_dir}")
            continue
        rv = _process(run, indoor_c0, indoor_c1, outdoor_c0, outdoor_c1)
        out_bin = out_dir / f"{spec.name}.bin"
        out_json = out_dir / f"{spec.name}.json"
        write_test_vector(rv, out_bin, out_json,
                          spec=spec, git_sha=git_sha, gen_utc=gen_utc)
        verify(out_bin, out_json, rv)
        rel = out_bin.relative_to(repo_root)
        print(f"  WROTE {rel}")
        print(f"    n_samples={len(rv.mag_g)}  n_peaks={len(rv.peaks)}  "
              f"R={rv.roughness_R:.4f}  surface={rv.surface_name}  "
              f"distance={rv.distance_ft:.2f} ft")
        print(f"    truth_ft={run.actual_distance_ft:.1f}  "
              f"motion_dur={rv.motion_duration_s:.1f}s  "
              f"({100*rv.motion_fraction:.0f}% of {rv.total_duration_s:.1f}s)")
        print(f"    bytes={out_bin.stat().st_size}  verify=OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

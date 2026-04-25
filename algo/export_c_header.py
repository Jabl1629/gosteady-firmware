"""Emit `src/algo/gosteady_algo_params.h` from the locked V1 algorithm.

This is the single source of truth for the parameters the M10 C port
must implement. Re-running this script regenerates the header; the C
code never carries hand-edited constants.

Three categories of value end up in the header:

  1. **Design constants** — chosen at the algorithm-architecture level
     and locked as part of M9 Phase 4 (auto-surface roughness adjustment,
     2026-04-25). Filter cutoffs, Schmitt thresholds, motion-gate
     thresholds, R classifier τ, sample rate. Defined here in one place
     and mirrored from `algo/distance_estimator.py` defaults.

  2. **Filter coefficients** — Butterworth SOS computed by scipy at the
     locked sample rate, converted to CMSIS-DSP `arm_biquad_cascade_df2T_f32`
     storage layout. See "CMSIS sign convention" below.

  3. **Per-surface stride coefficients** — fitted from the available
     dataset. Indoor coefficients fit on all valid indoor polished-concrete
     walks; outdoor on all valid outdoor concrete sidewalk walks. Not LOO —
     the deployment build uses the full fit. Re-running with new data
     (e.g. v1.5 carpet captures) regenerates these freshly.

CMSIS sign convention
---------------------
scipy SOS rows are `[b0, b1, b2, a0, a1, a2]` with the recurrence
`y[n] = b0·x[n] + b1·x[n-1] + b2·x[n-2] - a1·y[n-1] - a2·y[n-2]`
(a0 == 1 after normalization).

CMSIS-DSP `arm_biquad_cascade_df2T_f32` expects per-stage
`{b0, b1, b2, a1, a2}` (5 floats, a0 implicit = 1). **The CMSIS coefficient
storage already incorporates the sign of the feedback terms** — the
recurrence inside CMSIS is written `+ a1·y[n] + a2·y[n]`, so the stored
a1/a2 must be the NEGATION of scipy's a1/a2. We do that conversion
here; the emitted header is drop-in for CMSIS.

The Python `algo/filters.py` cross-checks against scipy's standard
`y - a1·y - a2·y` form (no sign flip). The C port using these emitted
coefficients with CMSIS will produce the same output as the Python
pipeline because the sign flip is absorbed into the stored constants.

Run from repo root:
    algo/venv/bin/python3 -m algo.export_c_header
"""

from __future__ import annotations

import datetime as _dt
import subprocess
from pathlib import Path

import numpy as np
from scipy import signal as _scipy_signal

from .data_loader import iter_walks, load_capture_day
from .distance_estimator import StepBasedDistanceEstimator
from .filters import butterworth_hp, butterworth_lp
from .motion_gate import MotionGate
from .roughness import inter_peak_rms_g
from .step_detector import Peak, StepDetector


# =====================================================================
# Locked design constants (mirror algo/distance_estimator.py defaults)
# =====================================================================

FS_HZ = 100.0

HP_CUTOFF_HZ = 0.2
LP_CUTOFF_HZ = 5.0
FILTER_ORDER = 2

PEAK_ENTER_G = 0.02
PEAK_EXIT_G = 0.005
PEAK_MIN_GAP_S = 0.5
PEAK_MAX_ACTIVE_S = 5.0

GATE_WINDOW_S = 0.5
GATE_ENTER_G = 0.01
GATE_EXIT_G = 0.005
GATE_EXIT_HOLD_S = 2.0

ROUGH_HALF_WINDOW_S = 0.2

# Auto-surface classifier — hard threshold on motion-gated inter-peak
# RMS R. Locked 2026-04-25 from `algo/run_auto_surface.py` (median
# midpoint between indoor and outdoor R distributions on n=16 walks).
R_THRESHOLD = 0.245

# Algorithm version string stamped into the firmware build. Bump when
# any locked constant above changes OR when the M10 C port lands.
ALGO_VERSION_STR = "0.6.0-algo-v1"


# =====================================================================
# Per-surface stride coefficient fit — uses all available walks
# =====================================================================

INDOOR_DATASET = "raw_sessions/2026-04-23"
OUTDOOR_DATASET = "raw_sessions/2026-04-25"


def _process_run(run) -> dict:
    """Mirror of `run_auto_surface.process()` — single source of truth
    for what features get extracted per walk. Kept inline here rather
    than imported because we want this script to be the *recipe* the
    firmware C port must reproduce."""
    fs = run.derived_rate_hz or FS_HZ
    hp = butterworth_hp(HP_CUTOFF_HZ, fs=fs, order=FILTER_ORDER)
    lp = butterworth_lp(LP_CUTOFF_HZ, fs=fs, order=FILTER_ORDER)
    det = StepDetector(
        fs=fs,
        enter_threshold_g=PEAK_ENTER_G,
        exit_threshold_g=PEAK_EXIT_G,
        min_gap_s=PEAK_MIN_GAP_S,
        max_active_s=PEAK_MAX_ACTIVE_S,
    )
    gate = MotionGate(
        fs=fs,
        window_samples=int(round(GATE_WINDOW_S * fs)),
        enter_threshold=GATE_ENTER_G,
        exit_threshold=GATE_EXIT_G,
        exit_hold_samples=int(round(GATE_EXIT_HOLD_S * fs)),
    )
    mag_g = run.accel_mag_g
    hp.init_steady(float(mag_g[0]) - 1.0)
    n = len(mag_g)
    mag_lp = np.empty(n, dtype=np.float64)
    motion_mask = np.zeros(n, dtype=bool)
    peaks: list[Peak] = []
    for i, x in enumerate(mag_g):
        v_hp = hp.step(float(x) - 1.0)
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
        half_window_s=ROUGH_HALF_WINDOW_S,
        motion_mask=motion_mask,
    )
    return {"run": run, "peaks": peaks, "R": R}


def _fit_surface_coeffs(walks: list[dict]) -> tuple[float, float]:
    """V1 single-feature fit: `stride_ft = c0 + c1·amp_g`.

    Per-run aggregate: `distance_r = c0·N_r + c1·Σamp_r`. Ridge
    regression matches `algo/run_auto_surface.py::fit_surface_coeffs`.
    Returns (c0, c1).
    """
    rows = []
    y = []
    for d in walks:
        peaks: list[Peak] = d["peaks"]
        sum_amp = sum(p.amplitude_g for p in peaks)
        rows.append([float(len(peaks)), float(sum_amp)])
        y.append(d["run"].actual_distance_ft)
    X = np.array(rows, dtype=np.float64)
    y = np.array(y, dtype=np.float64)
    A = X.T @ X + 1e-3 * np.eye(2)
    c = np.linalg.solve(A, X.T @ y)
    return float(c[0]), float(c[1])


# =====================================================================
# scipy SOS → CMSIS-DSP storage layout
# =====================================================================


def _sos_to_cmsis(sos: np.ndarray) -> np.ndarray:
    """Convert a scipy SOS array (n, 6) to CMSIS-DSP df2T storage (n, 5).

    scipy: per-row [b0, b1, b2, a0, a1, a2] with a0 == 1.
    CMSIS:  per-row [b0, b1, b2, -a1, -a2] (a0 implicit; sign of feedback
            terms negated because CMSIS recurrence is `+ a1·y + a2·y`).
    """
    sos = np.asarray(sos, dtype=np.float64)
    if sos.ndim != 2 or sos.shape[1] != 6:
        raise ValueError(f"sos must be (n, 6); got {sos.shape}")
    if not np.allclose(sos[:, 3], 1.0):
        raise ValueError("a0 != 1 in input SOS — scipy normalizes; check input")
    out = np.empty((sos.shape[0], 5), dtype=np.float64)
    out[:, 0] = sos[:, 0]   # b0
    out[:, 1] = sos[:, 1]   # b1
    out[:, 2] = sos[:, 2]   # b2
    out[:, 3] = -sos[:, 4]  # -a1
    out[:, 4] = -sos[:, 5]  # -a2
    return out


# =====================================================================
# Header emission
# =====================================================================


def _git_sha(repo_root: Path) -> str:
    try:
        out = subprocess.run(
            ["git", "-C", str(repo_root), "rev-parse", "--short=12", "HEAD"],
            capture_output=True, text=True, check=True,
        )
        return out.stdout.strip()
    except Exception:
        return "unknown"


def _git_dirty(repo_root: Path) -> bool:
    try:
        out = subprocess.run(
            ["git", "-C", str(repo_root), "status", "--porcelain"],
            capture_output=True, text=True, check=True,
        )
        return bool(out.stdout.strip())
    except Exception:
        return False


def _format_float_array(name: str, values: np.ndarray, per_line: int = 5) -> str:
    """Emit a `static const float NAME[N] = { ... };` block."""
    flat = np.asarray(values, dtype=np.float32).ravel()
    lines = []
    for i in range(0, len(flat), per_line):
        chunk = flat[i : i + per_line]
        lines.append("    " + ", ".join(f"{x: .9e}f" for x in chunk) + ",")
    body = "\n".join(lines).rstrip(",")
    return (
        f"static const float {name}[{len(flat)}] = {{\n"
        f"{body}\n"
        f"}};"
    )


def _emit_header(
    *,
    hp_cmsis: np.ndarray,
    lp_cmsis: np.ndarray,
    indoor_c0: float,
    indoor_c1: float,
    outdoor_c0: float,
    outdoor_c1: float,
    n_indoor_walks: int,
    n_outdoor_walks: int,
    git_sha: str,
    git_dirty: bool,
    gen_utc: str,
) -> str:
    gate_window_samples = int(round(GATE_WINDOW_S * FS_HZ))
    gate_exit_hold_samples = int(round(GATE_EXIT_HOLD_S * FS_HZ))
    rough_half_window_samples = int(round(ROUGH_HALF_WINDOW_S * FS_HZ))
    dirty_marker = " (DIRTY WORKING TREE)" if git_dirty else ""

    parts: list[str] = [
        "/*",
        " * gosteady_algo_params.h — V1 distance-estimator coefficients (auto-generated).",
        " *",
        " * DO NOT EDIT BY HAND. Regenerate with:",
        " *     algo/venv/bin/python3 -m algo.export_c_header",
        " *",
        " * Generator:        algo/export_c_header.py",
        f" * Generated (UTC):  {gen_utc}",
        f" * Git commit:       {git_sha}{dirty_marker}",
        " * Algorithm:        V1 — auto-surface roughness adjustment",
        f" * Algo version:     {ALGO_VERSION_STR}",
        " *",
        " * Coefficient provenance:",
        f" *   indoor stride fit on {n_indoor_walks} valid walks from {INDOOR_DATASET}/",
        f" *   outdoor stride fit on {n_outdoor_walks} valid walks from {OUTDOOR_DATASET}/",
        " *",
        " * Filter coefficients are Butterworth SOS in CMSIS-DSP",
        " * arm_biquad_cascade_df2T_f32 storage layout: per stage",
        " * { b0, b1, b2, -a1, -a2 } (a0 implicit = 1; feedback-term signs",
        " * negated per CMSIS convention).",
        " */",
        "",
        "#ifndef GOSTEADY_ALGO_PARAMS_H",
        "#define GOSTEADY_ALGO_PARAMS_H",
        "",
        "#include <stdint.h>",
        "",
        "/* === Sampling === */",
        f"#define GS_FS_HZ                          {FS_HZ:.1f}f",
        f"#define GS_FS_HZ_INT                      {int(FS_HZ)}",
        "",
        "/* === HP filter (gravity removal) === */",
        f"#define GS_HP_CUTOFF_HZ                   {HP_CUTOFF_HZ:.3f}f",
        f"#define GS_HP_NUM_STAGES                  {hp_cmsis.shape[0]}",
        _format_float_array("gs_hp_coeffs", hp_cmsis),
        "",
        "/* === LP filter (step shaping) === */",
        f"#define GS_LP_CUTOFF_HZ                   {LP_CUTOFF_HZ:.3f}f",
        f"#define GS_LP_NUM_STAGES                  {lp_cmsis.shape[0]}",
        _format_float_array("gs_lp_coeffs", lp_cmsis),
        "",
        "/* === Step detector (Schmitt-trigger peak FSM) === */",
        f"#define GS_PEAK_ENTER_G                   {PEAK_ENTER_G:.4f}f",
        f"#define GS_PEAK_EXIT_G                    {PEAK_EXIT_G:.4f}f",
        f"#define GS_PEAK_MIN_GAP_S                 {PEAK_MIN_GAP_S:.3f}f",
        f"#define GS_PEAK_MIN_GAP_SAMPLES           {int(round(PEAK_MIN_GAP_S * FS_HZ))}",
        f"#define GS_PEAK_MAX_ACTIVE_S              {PEAK_MAX_ACTIVE_S:.3f}f",
        f"#define GS_PEAK_MAX_ACTIVE_SAMPLES        {int(round(PEAK_MAX_ACTIVE_S * FS_HZ))}",
        "",
        "/* === Motion gate (running-σ with hysteresis) === */",
        f"#define GS_GATE_WINDOW_S                  {GATE_WINDOW_S:.3f}f",
        f"#define GS_GATE_WINDOW_SAMPLES            {gate_window_samples}",
        f"#define GS_GATE_ENTER_G                   {GATE_ENTER_G:.4f}f",
        f"#define GS_GATE_EXIT_G                    {GATE_EXIT_G:.4f}f",
        f"#define GS_GATE_EXIT_HOLD_S               {GATE_EXIT_HOLD_S:.3f}f",
        f"#define GS_GATE_EXIT_HOLD_SAMPLES         {gate_exit_hold_samples}",
        "",
        "/* === Roughness metric (motion-gated inter-peak RMS) === */",
        f"#define GS_ROUGH_HALF_WINDOW_S            {ROUGH_HALF_WINDOW_S:.3f}f",
        f"#define GS_ROUGH_HALF_WINDOW_SAMPLES      {rough_half_window_samples}",
        "",
        "/* === Surface classifier === */",
        f"#define GS_R_THRESHOLD                    {R_THRESHOLD:.4f}f",
        "",
        "typedef enum {",
        "    GS_SURFACE_INDOOR  = 0,",
        "    GS_SURFACE_OUTDOOR = 1,",
        "    GS_NUM_SURFACES    = 2,",
        "} gs_surface_t;",
        "",
        "/* === Per-surface stride coefficients ===",
        " * Inference per peak:",
        " *     stride_ft = gs_stride_intercept_ft[surface]",
        " *               + gs_stride_amp_coeff[surface] * peak_amp_g",
        " * Per-session distance is the sum over emitted peaks, clamped to >= 0.",
        " */",
        "static const float gs_stride_intercept_ft[GS_NUM_SURFACES] = {",
        f"    {indoor_c0:+.9e}f,  /* GS_SURFACE_INDOOR  */",
        f"    {outdoor_c0:+.9e}f,  /* GS_SURFACE_OUTDOOR */",
        "};",
        "",
        "static const float gs_stride_amp_coeff[GS_NUM_SURFACES] = {",
        f"    {indoor_c1:+.9e}f,  /* GS_SURFACE_INDOOR  */",
        f"    {outdoor_c1:+.9e}f,  /* GS_SURFACE_OUTDOOR */",
        "};",
        "",
        "/* === Provenance strings (also include in telemetry / logs) === */",
        f'#define GS_ALGO_VERSION_STR               "{ALGO_VERSION_STR}"',
        f'#define GS_ALGO_PARAMS_GIT_SHA            "{git_sha}{dirty_marker}"',
        f'#define GS_ALGO_PARAMS_GEN_UTC            "{gen_utc}"',
        "",
        "#endif /* GOSTEADY_ALGO_PARAMS_H */",
        "",
    ]
    return "\n".join(parts)


# =====================================================================
# Entry point
# =====================================================================


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    out_dir = repo_root / "src" / "algo"
    out_path = out_dir / "gosteady_algo_params.h"

    # 1. Filter coefficients
    hp_sos = _scipy_signal.butter(
        FILTER_ORDER, HP_CUTOFF_HZ, btype="high", fs=FS_HZ, output="sos"
    )
    lp_sos = _scipy_signal.butter(
        FILTER_ORDER, LP_CUTOFF_HZ, btype="low", fs=FS_HZ, output="sos"
    )
    hp_cmsis = _sos_to_cmsis(hp_sos)
    lp_cmsis = _sos_to_cmsis(lp_sos)

    # 2. Per-surface stride coefficients (fit on full available data)
    print(f"Loading indoor walks  from {INDOOR_DATASET}/ ...")
    indoor_runs = load_capture_day(repo_root / INDOOR_DATASET)
    indoor_walks_runs = [r for r in iter_walks(indoor_runs) if r.valid]
    indoor_walks = [_process_run(r) for r in indoor_walks_runs]
    indoor_c0, indoor_c1 = _fit_surface_coeffs(indoor_walks)
    print(f"  fitted on {len(indoor_walks)} walks: "
          f"stride_ft = {indoor_c0:+.4f} + {indoor_c1:+.4f}·amp_g")

    print(f"Loading outdoor walks from {OUTDOOR_DATASET}/ ...")
    outdoor_runs = load_capture_day(repo_root / OUTDOOR_DATASET)
    outdoor_walks_runs = [r for r in iter_walks(outdoor_runs) if r.valid]
    outdoor_walks = [_process_run(r) for r in outdoor_walks_runs]
    outdoor_c0, outdoor_c1 = _fit_surface_coeffs(outdoor_walks)
    print(f"  fitted on {len(outdoor_walks)} walks: "
          f"stride_ft = {outdoor_c0:+.4f} + {outdoor_c1:+.4f}·amp_g")

    # 3. Provenance
    git_sha = _git_sha(repo_root)
    git_dirty = _git_dirty(repo_root)
    gen_utc = _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds")

    # 4. Emit
    out_dir.mkdir(parents=True, exist_ok=True)
    header_text = _emit_header(
        hp_cmsis=hp_cmsis,
        lp_cmsis=lp_cmsis,
        indoor_c0=indoor_c0,
        indoor_c1=indoor_c1,
        outdoor_c0=outdoor_c0,
        outdoor_c1=outdoor_c1,
        n_indoor_walks=len(indoor_walks),
        n_outdoor_walks=len(outdoor_walks),
        git_sha=git_sha,
        git_dirty=git_dirty,
        gen_utc=gen_utc,
    )
    out_path.write_text(header_text)
    print(f"\nWrote {out_path.relative_to(repo_root)} "
          f"({len(header_text)} bytes, git={git_sha}"
          f"{' dirty' if git_dirty else ''})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

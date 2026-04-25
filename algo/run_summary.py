"""Comprehensive per-run summary table for anomaly scanning.

One row per session across both capture days. Includes ground truth,
algorithm-derived stats, and the discrepancies between them. Predictions
use per-surface LOO (each fold's prediction comes from a model trained
on the other 7 walks of that surface). Robustness-gate runs (stationary,
stumble) get predictions from the final per-surface model fit on all 8
walks of their respective surfaces.

Columns:
    surf      indoor / outdoor
    run       run_idx from POST-WALK
    type      run_type (normal/pickup/stumble/stationary_baseline)
    speed     intended_speed
    truth_ft  actual_distance_ft from POST-WALK
    dur_s     total session duration (from .dat body t_ms range)
    motion_s  in-motion duration from the 500 ms running-σ gate
    mot_%     motion / total
    man_st    manual step count
    det_st    detected peak count (V1 loose Schmitt detector)
    ratio     det_st / man_st
    stride    truth_ft / man_st  (manual stride length, ft/step)
    σ_a_HP    σ of |a|_HP over middle half (signal magnitude)
    R_ipr     inter-peak RMS roughness metric
    R_hflf    HF/LF PSD ratio roughness metric
    pred_ft   LOO predicted distance
    err%      (pred - truth) / truth × 100

Run from repo root:
    algo/venv/bin/python3 -m algo.run_summary
"""

from __future__ import annotations

from typing import Sequence

import numpy as np

from .data_loader import Run, iter_walks, load_capture_day
from .distance_estimator import StepBasedDistanceEstimator
from .filters import butterworth_hp, butterworth_lp
from .motion_gate import MotionGate
from .roughness import hf_lf_ratio, inter_peak_rms_g
from .step_detector import Peak, StepDetector


def _fmt(v, fmt="%.2f", missing="-"):
    if v is None:
        return missing
    if isinstance(v, float) and not np.isfinite(v):
        return missing
    if isinstance(v, str):
        return v
    return fmt % v


def process(run: Run) -> dict:
    """Run the V1 streaming pipeline + roughness metrics on one session."""
    fs = run.derived_rate_hz or 100.0
    hp = butterworth_hp(0.2, fs=fs)
    lp = butterworth_lp(5.0, fs=fs)
    det = StepDetector(fs=fs, enter_threshold_g=0.02,
                       exit_threshold_g=0.005, min_gap_s=0.5)
    gate = MotionGate(fs=fs, window_samples=int(0.5 * fs),
                      enter_threshold=0.01, exit_threshold=0.005,
                      exit_hold_samples=int(2.0 * fs))
    mag_g = run.accel_mag_g
    hp.init_steady(float(mag_g[0]) - 1.0)
    n = len(mag_g)
    mag_hp = np.empty(n)
    mag_lp = np.empty(n)
    motion_mask = np.zeros(n, dtype=bool)
    peaks: list[Peak] = []
    for i, x in enumerate(mag_g):
        v_hp = hp.step(x - 1.0)
        mag_hp[i] = v_hp
        v_lp = lp.step(v_hp)
        mag_lp[i] = v_lp
        motion_mask[i] = gate.step(v_hp)
        p = det.step(v_lp)
        if p is not None:
            peaks.append(p)

    # σ |a|_HP over motion-gated samples only (replaces the
    # "middle half" hack — properly excludes settling tails).
    motion_segment = mag_hp[motion_mask]
    sigma_walk = (float(motion_segment.std())
                  if len(motion_segment) > 10 else float("nan"))

    # Total duration from body t_ms
    t = run.imu["t_ms"]
    total_dur_s = (int(t.max()) - int(t.min())) / 1000.0

    return {
        "run": run,
        "fs": fs,
        "peaks": peaks,
        "motion_mask": motion_mask,
        "total_dur_s": total_dur_s,
        "motion_dur_s": gate.motion_duration_s,
        "motion_frac": gate.motion_fraction,
        "sigma_walk_g": sigma_walk,
        "ipr": inter_peak_rms_g(mag_lp,
                                [p.sample_idx for p in peaks], fs,
                                motion_mask=motion_mask),
        "hflf": hf_lf_ratio(mag_hp, fs, motion_mask=motion_mask),
    }


def loo_predict_per_surface(walks: Sequence[Run], target_idx: int) -> float:
    """LOO prediction: train on all OTHER walks of the same surface,
    predict the target."""
    train = [r for j, r in enumerate(walks) if j != target_idx]
    m = StepBasedDistanceEstimator()
    m.fit(train)
    return float(max(m.predict(walks[target_idx]).distance_ft, 0.0))


def final_predict(walks: Sequence[Run], target: Run) -> float:
    m = StepBasedDistanceEstimator()
    m.fit(list(walks))
    return float(max(m.predict(target).distance_ft, 0.0))


def main() -> int:
    indoor_runs = load_capture_day("raw_sessions/2026-04-23")
    outdoor_runs = load_capture_day("raw_sessions/2026-04-25")
    indoor_walks = [r for r in iter_walks(indoor_runs) if r.valid]
    outdoor_walks = [r for r in iter_walks(outdoor_runs) if r.valid]
    indoor_gates = [r for r in indoor_runs
                    if r.run_type in ("stationary_baseline", "stumble")
                    and r.valid]
    outdoor_gates = [r for r in outdoor_runs
                     if r.run_type in ("stationary_baseline", "stumble")
                     and r.valid]

    # Process all runs once
    rows = []

    # ---- Indoor walks (LOO per-surface predictions) ----
    for i, r in enumerate(indoor_walks):
        d = process(r)
        d["surface"] = "indoor"
        d["pred_ft"] = loo_predict_per_surface(indoor_walks, i)
        rows.append(d)
    # Indoor robustness gates (final-model predictions)
    for r in indoor_gates:
        d = process(r)
        d["surface"] = "indoor"
        d["pred_ft"] = final_predict(indoor_walks, r)
        rows.append(d)
    # ---- Outdoor walks ----
    for i, r in enumerate(outdoor_walks):
        d = process(r)
        d["surface"] = "outdoor"
        d["pred_ft"] = loo_predict_per_surface(outdoor_walks, i)
        rows.append(d)
    # Outdoor gates
    for r in outdoor_gates:
        d = process(r)
        d["surface"] = "outdoor"
        d["pred_ft"] = final_predict(outdoor_walks, r)
        rows.append(d)

    # ---- Print table ----
    hdr = (f"  {'surf':<7} {'run':>3} {'type':<11} {'speed':<6} "
           f"{'truth':>5} {'dur':>5} {'mot':>5} {'mot%':>4} "
           f"{'man':>3} {'det':>3} {'rat':>4} {'stride':>6} "
           f"{'σaHP':>5} {'R_ipr':>5} {'R_hflf':>6} "
           f"{'pred':>6} {'err%':>6}")
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))
    for d in rows:
        r = d["run"]
        truth = r.actual_distance_ft
        manual = r.manual_step_count
        det = len(d["peaks"])
        ratio = det / manual if (np.isfinite(manual) and manual > 0) else float("nan")
        stride = (truth / manual
                  if (np.isfinite(manual) and manual > 0)
                  else float("nan"))
        err_pct = ((d["pred_ft"] - truth) / truth * 100.0
                   if truth > 0 else float("nan"))
        type_short = {
            "normal": "normal",
            "pickup": "pickup",
            "stumble": "stumble",
            "stationary_baseline": "stat_base",
        }.get(r.run_type, r.run_type[:11])
        print(f"  {d['surface']:<7} {r.run_idx:>3} {type_short:<11} "
              f"{r.intended_speed:<6} "
              f"{_fmt(truth, '%5.1f'):>5} "
              f"{_fmt(d['total_dur_s'], '%5.1f'):>5} "
              f"{_fmt(d['motion_dur_s'], '%5.1f'):>5} "
              f"{_fmt(100*d['motion_frac'], '%4.0f'):>4} "
              f"{_fmt(manual, '%3.0f'):>3} "
              f"{det:>3} "
              f"{_fmt(ratio, '%4.2f'):>4} "
              f"{_fmt(stride, '%6.2f'):>6} "
              f"{_fmt(d['sigma_walk_g'], '%5.3f'):>5} "
              f"{_fmt(d['ipr'], '%5.3f'):>5} "
              f"{_fmt(d['hflf'], '%6.3f'):>6} "
              f"{_fmt(d['pred_ft'], '%6.2f'):>6} "
              f"{_fmt(err_pct, '%+6.1f'):>6}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

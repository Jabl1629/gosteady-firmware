"""Does bumping min_gap (to fix step count) disturb the validated distance
estimate? Re-runs the shipped auto-surface LOO (hard tau=0.245) at several
min_gap values and reports pooled distance MAPE + total peak count.

Run: algo/venv/bin/python3 -m algo._min_gap_distance_probe
"""
from __future__ import annotations

import warnings
import numpy as np

from .data_loader import iter_walks, load_capture_day
from .evaluator import aggregate
from .filters import butterworth_hp, butterworth_lp
from .motion_gate import MotionGate
from .roughness import inter_peak_rms_g
from .step_detector import Peak, StepDetector
from .run_auto_surface import hard_threshold_pick, loocv_auto_surface

warnings.simplefilter("ignore")


def process(run, min_gap_s: float) -> dict:
    fs = run.derived_rate_hz or 100.0
    hp = butterworth_hp(0.2, fs=fs)
    lp = butterworth_lp(5.0, fs=fs)
    det = StepDetector(fs=fs, enter_threshold_g=0.02, exit_threshold_g=0.005,
                       min_gap_s=min_gap_s)
    gate = MotionGate(fs=fs, window_samples=int(0.5 * fs),
                      enter_threshold=0.01, exit_threshold=0.005,
                      exit_hold_samples=int(2.0 * fs))
    mag_g = run.accel_mag_g
    hp.init_steady(float(mag_g[0]) - 1.0)
    n = len(mag_g)
    mag_lp = np.empty(n)
    motion_mask = np.zeros(n, dtype=bool)
    peaks: list[Peak] = []
    for i, x in enumerate(mag_g):
        v_hp = hp.step(x - 1.0)
        v_lp = lp.step(v_hp)
        mag_lp[i] = v_lp
        motion_mask[i] = gate.step(v_hp)
        p = det.step(v_lp)
        if p is not None:
            peaks.append(p)
    R = inter_peak_rms_g(mag_lp, [p.sample_idx for p in peaks], fs,
                         motion_mask=motion_mask)
    return {"run": run, "peaks": peaks, "R": R}


def main() -> int:
    indoor_runs = load_capture_day("raw_sessions/2026-04-23")
    outdoor_runs = load_capture_day("raw_sessions/2026-04-25")
    indoor_walks = [r for r in iter_walks(indoor_runs) if r.valid]
    outdoor_walks = [r for r in iter_walks(outdoor_runs) if r.valid]

    print(f"{'min_gap':>8} {'pooled dist MAPE':>17} {'indoor':>8} {'outdoor':>8} "
          f"{'total peaks':>12}")
    for gap in [0.5, 0.7, 0.9, 1.1]:
        indoor_p = [process(r, gap) for r in indoor_walks]
        outdoor_p = [process(r, gap) for r in outdoor_walks]
        tags = ["indoor"] * len(indoor_p) + ["outdoor"] * len(outdoor_p)
        clf = lambda R, i, o: hard_threshold_pick(R, 0.245, i, o)
        per_fold, _ = loocv_auto_surface(indoor_p, outdoor_p, clf, "hard")
        agg_in = aggregate([rr for t, rr in zip(tags, per_fold) if t == "indoor"])
        agg_out = aggregate([rr for t, rr in zip(tags, per_fold) if t == "outdoor"])
        agg_pool = aggregate(per_fold)
        total_peaks = sum(len(d["peaks"]) for d in indoor_p + outdoor_p)
        print(f"{gap:>8.1f} {100*agg_pool.distance_mape:>16.1f}% "
              f"{100*agg_in.distance_mape:>7.1f}% {100*agg_out.distance_mape:>7.1f}% "
              f"{total_peaks:>12}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

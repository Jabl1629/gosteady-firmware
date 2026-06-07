"""Throwaway probe: detector peak-count vs manual step count across all
labeled walks, plus a min-gap sweep. Answers: is the over-count a stable
ratio (cheap fix) or variable (needs a dedicated step counter)?

Run: algo/venv/bin/python3 -m algo._stepcount_probe
"""
from __future__ import annotations

import warnings
import numpy as np

from .data_loader import load_capture_day
from .filters import butterworth_hp, butterworth_lp
from .step_detector import StepDetector

warnings.simplefilter("ignore")

DAYS = ["raw_sessions/2026-04-23", "raw_sessions/2026-04-25", "raw_sessions/2026-05-05"]

# Current shipped params (gosteady_algo_params.h)
HP_HZ, LP_HZ, ORDER = 0.2, 5.0, 2
ENTER, EXIT, MAXACT = 0.02, 0.005, 5.0
GAP_SWEEP = [0.5, 0.7, 0.9, 1.1, 1.3, 1.5]


def detect_count(mag_g: np.ndarray, fs: float, min_gap_s: float) -> int:
    hp = butterworth_hp(HP_HZ, fs=fs, order=ORDER)
    lp = butterworth_lp(LP_HZ, fs=fs, order=ORDER)
    hp.init_steady(float(mag_g[0]) - 1.0)
    det = StepDetector(fs=fs, enter_threshold_g=ENTER, exit_threshold_g=EXIT,
                       min_gap_s=min_gap_s, max_active_s=MAXACT)
    n = 0
    for i in range(len(mag_g)):
        v_hp = hp.step(float(mag_g[i]) - 1.0)
        v_lp = lp.step(v_hp)
        if det.step(v_lp) is not None:
            n += 1
    return n


def main() -> int:
    rows = []
    for day in DAYS:
        try:
            runs = load_capture_day(day)
        except Exception as e:
            print(f"skip {day}: {e}")
            continue
        for r in runs:
            man = r.manual_step_count
            if not np.isfinite(man) or man <= 0 or not r.is_walk:
                continue
            fs = r.derived_rate_hz or 100.0
            mag = r.accel_mag_g
            cur = detect_count(mag, fs, 0.5)
            sweep = {g: detect_count(mag, fs, g) for g in GAP_SWEEP}
            rows.append({
                "day": day.split("/")[-1], "idx": r.run_idx,
                "type": r.run_type, "speed": r.intended_speed,
                "dist": r.actual_distance_ft, "manual": int(man),
                "cur": cur, "sweep": sweep,
            })

    if not rows:
        print("no labeled walks found")
        return 1

    print(f"\n{'day':>10} {'idx':>3} {'type':<12} {'speed':<6} "
          f"{'man':>4} {'cur(0.5)':>8} {'ratio':>6}  " +
          " ".join(f"g{g}".rjust(5) for g in GAP_SWEEP))
    ratios = []
    for x in rows:
        ratio = x["cur"] / x["manual"]
        ratios.append(ratio)
        print(f"{x['day']:>10} {x['idx']:>3} {x['type']:<12} {x['speed']:<6} "
              f"{x['manual']:>4} {x['cur']:>8} {ratio:>6.2f}  " +
              " ".join(str(x['sweep'][g]).rjust(5) for g in GAP_SWEEP))

    ratios = np.array(ratios)
    print(f"\nn walks with manual count: {len(rows)}")
    print(f"current-detector / manual ratio: "
          f"mean={ratios.mean():.2f}  median={np.median(ratios):.2f}  "
          f"std={ratios.std():.2f}  min={ratios.min():.2f}  max={ratios.max():.2f}")

    # If we applied a single best fixed divisor, what's the residual MAPE on count?
    best_div = ratios.mean()
    man = np.array([x["manual"] for x in rows], dtype=float)
    cur = np.array([x["cur"] for x in rows], dtype=float)
    corr = np.corrcoef(cur, man)[0, 1]
    div_pred = cur / best_div
    div_mape = np.mean(np.abs(div_pred - man) / man) * 100
    print(f"corr(cur_peaks, manual) = {corr:.3f}")
    print(f"fixed-divisor (÷{best_div:.2f}) residual step-count MAPE = {div_mape:.1f}%")

    # Best single min_gap across the sweep (minimize count MAPE vs manual)
    print("\nmin_gap sweep — step-count MAPE vs manual (no divisor):")
    for g in GAP_SWEEP:
        pred = np.array([x["sweep"][g] for x in rows], dtype=float)
        mape = np.mean(np.abs(pred - man) / man) * 100
        rr = pred / man
        print(f"  min_gap={g:.1f}s  MAPE={mape:5.1f}%  ratio mean={rr.mean():.2f} std={rr.std():.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

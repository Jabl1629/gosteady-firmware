"""A/B comparison — peak-based (Path A) vs energy-based (Path B)
distance estimators on the 2026-04-23 shakedown dataset.

Four configurations, same LOO harness, same robustness gates:

  A1. Peak-based, single feature (amp)           — matches prior-work structure
  A2. Peak-based, loose thresholds (enter=0.02g) — catches compound impulses
  B1. Energy, E only                             — D = a·E + c
  B2. Energy + duration, E + T                   — D = a·E + b·T + c
  B3. Duration only, T                           — sanity baseline: if this
                                                    wins, speed is so uniform
                                                    that just knowing how
                                                    long the walker moved
                                                    is enough to predict
                                                    distance.

Run from repo root:
    algo/venv/bin/python3 -m algo.run_path_comparison
"""

from __future__ import annotations

import numpy as np

from .data_loader import load_capture_day
from .distance_estimator import StepBasedDistanceEstimator
from .energy_estimator import EnergyDistanceEstimator
from .evaluator import loocv


CONFIGS = [
    ("A1  peak-based, single feature (amp)",
     lambda: StepBasedDistanceEstimator(features=("amplitude_g",))),
    ("A2  peak-based, loose thresholds (enter=0.02g, min_gap=0.5s)",
     lambda: StepBasedDistanceEstimator(
         features=("amplitude_g",),
         peak_enter_g=0.02, peak_exit_g=0.005, peak_min_gap_s=0.5)),
    ("B1  energy only  (D = a·E + c)",
     lambda: EnergyDistanceEstimator(features=("E_motion_g2s",))),
    ("B2  energy + duration  (D = a·E + b·T + c)",
     lambda: EnergyDistanceEstimator(features=("E_motion_g2s", "T_motion_s"))),
    ("B3  duration only  (D = b·T + c) — sanity baseline",
     lambda: EnergyDistanceEstimator(features=("T_motion_s",))),
]


def main() -> int:
    runs = load_capture_day("raw_sessions/2026-04-23")
    print(f"Loaded {len(runs)} runs from raw_sessions/2026-04-23\n")

    summary_rows = []
    for label, factory in CONFIGS:
        print("=" * 80)
        print(label)
        print("=" * 80)
        report = loocv(runs, factory)

        # Per-fold line
        print(f"  {'run':>3}  {'truth':>6}  {'pred':>6}  {'err%':>6}  {'notes'}")
        for rr in report.per_fold:
            err_pct = (f"{100*rr.pct_err:+6.1f}"
                       if np.isfinite(rr.pct_err) else "     -")
            note_parts = []
            if rr.pred.step_count is not None:
                tm = (int(rr.run.manual_step_count)
                      if np.isfinite(rr.run.manual_step_count) else "-")
                note_parts.append(f"steps {rr.pred.step_count}/{tm}")
            d = rr.pred.diagnostics
            if "energy_motion_g2s" in d:
                note_parts.append(f"E={d['energy_motion_g2s']:.2f}")
            if "motion_duration_s" in d:
                note_parts.append(f"T={d['motion_duration_s']:.1f}s")
            print(f"  {rr.run.run_idx:>3}  {rr.truth_ft:>6.1f}  "
                  f"{rr.pred.distance_ft:>6.2f}  {err_pct:>6}  "
                  f"{' '.join(note_parts)}")

        a = report.aggregate
        print(f"  → MAE {a.distance_mae_ft:.2f} ft, "
              f"MAPE {100*a.distance_mape:.1f}%, "
              f"worst {100*a.distance_worst_pct:.1f}%, "
              f"RMSE {a.distance_rmse_ft:.2f} ft")

        # Robustness gates
        for g in report.gates:
            mark = "PASS" if g.passed else "FAIL"
            print(f"  [{mark}] {g.reason}")

        # Final model formula + fit diagnostics
        final = factory()
        walks = [r for r in runs if r.is_walk
                 and np.isfinite(r.actual_distance_ft)
                 and r.actual_distance_ft > 0]
        final.fit(walks)
        if hasattr(final, "stride"):
            formula = final.stride.stride_formula()
            diag = final.stride.fit_diagnostics()
        else:
            formula = final.formula
            diag = final.fit_diagnostics()
        print(f"  model: {formula}")
        if diag:
            print(f"  fit: train R²={diag['r_squared_train']:.3f}, "
                  f"cond={diag.get('cond_number', float('nan')):.1f}")
        print()

        summary_rows.append(dict(
            label=label, mape=a.distance_mape, mae=a.distance_mae_ft,
            worst=a.distance_worst_pct, rmse=a.distance_rmse_ft,
            gates_passed=sum(1 for g in report.gates if g.passed),
            gates_total=len(report.gates),
        ))

    print("=" * 80)
    print("SUMMARY — ranked by MAPE")
    print("=" * 80)
    summary_rows.sort(key=lambda r: r["mape"])
    print(f"  {'config':<60}  {'MAPE':>6}  {'MAE':>5}  "
          f"{'worst':>6}  {'gates':>6}")
    for row in summary_rows:
        print(f"  {row['label']:<60}  "
              f"{100*row['mape']:>5.1f}%  "
              f"{row['mae']:>4.2f}ft  "
              f"{100*row['worst']:>5.1f}%  "
              f"{row['gates_passed']}/{row['gates_total']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Phase 3 evaluation harness.

Runs LOO CV on the 2026-04-23 shakedown dataset for the step-based
distance estimator. Reports:

  - Multi-feature regression (amplitude + duration + energy)
  - Single-feature regression (amplitude only) — reproduces the prior
    work's structure for A/B comparison
  - Peak-detection diagnostics (detected vs. manual step counts)
  - Motion-gate diagnostics (duration + in-motion fraction)
  - Robustness gate passes/fails (stationary, stumble)

Run from the repo root:
    algo/venv/bin/python3 -m algo.run_phase3
"""

from __future__ import annotations

import numpy as np

from .data_loader import load_capture_day
from .distance_estimator import StepBasedDistanceEstimator
from .evaluator import loocv


def _factory_multi():
    return StepBasedDistanceEstimator(
        features=("amplitude_g", "duration_s", "energy_g2s"),
    )


def _factory_single():
    return StepBasedDistanceEstimator(
        features=("amplitude_g",),
    )


def main() -> int:
    runs = load_capture_day("raw_sessions/2026-04-23")
    print(f"Loaded {len(runs)} runs from raw_sessions/2026-04-23\n")

    for label, factory in (
        ("MULTI-FEATURE  (amplitude + duration + energy)", _factory_multi),
        ("SINGLE-FEATURE (amplitude only, matches prior work)", _factory_single),
    ):
        print("=" * 70)
        print(label)
        print("=" * 70)
        report = loocv(runs, factory)
        print(report.to_text())
        print()

        # Introspect the final model's stride formula.
        final = factory()
        final.fit([r for r in runs if r.is_walk
                   and np.isfinite(r.actual_distance_ft)
                   and r.actual_distance_ft > 0])
        print("Final model (fit on all walks):")
        print(f"  {final.stride.stride_formula()}")
        diag = final.stride.fit_diagnostics()
        if diag:
            print(f"  training R² = {diag['r_squared_train']:.3f}  "
                  f"(n={diag['n_runs']} walks, cond={diag['cond_number']:.1f})")

        # Motion duration diagnostics per run.
        print("\nMotion-gate duration vs. total session time:")
        print(f"  {'run':>3}  {'type':<20}  {'total':>6}  {'motion':>6}  {'frac':>5}")
        for r in runs:
            p = final.predict(r)
            print(f"  {r.run_idx:>3}  {r.run_type:<20}  "
                  f"{p.diagnostics['total_duration_s']:>6.1f}  "
                  f"{p.diagnostics['motion_duration_s']:>6.1f}  "
                  f"{100 * p.diagnostics['motion_fraction']:>4.0f}%")
        print()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

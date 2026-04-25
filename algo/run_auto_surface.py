"""Auto-surface classification — the operator never picks a surface.

Hypothesis: the motion-gated roughness metric R cleanly biases each
walk toward its true surface (indoor median 0.18, outdoor median 0.31,
with a small overlap zone). If we use R as a soft classifier and
*select* between per-surface coefficient sets at inference, we get
the per-surface table's accuracy WITHOUT the operator surface-selection
step. This is the path to a single-button user experience.

Two flavors of classifier:

  HARD threshold:  R < τ  ⇒ indoor coeffs   else outdoor coeffs.
  SOFT blend:      w = sigmoid((R - τ) / scale)
                   blended = (1 - w)·indoor_coeffs + w·outdoor_coeffs

Per-fold protocol (LOO over all 16 walks):

  1. Hold out walk i; classify it by its R.
  2. Fit indoor coefficients on all OTHER indoor walks
     (excluding i if it's indoor).
  3. Fit outdoor coefficients on all OTHER outdoor walks
     (excluding i if it's outdoor).
  4. Predict the held-out walk using the classifier output.
  5. Score against ground truth.

This isolates the *classifier's* contribution — coefficients are
already as good as per-surface LOO can make them; we measure how
much accuracy we lose by replacing the operator's surface selection
with R-driven auto-classification.

Run from repo root:
    algo/venv/bin/python3 -m algo.run_auto_surface
"""

from __future__ import annotations

import numpy as np

from .data_loader import iter_walks, load_capture_day
from .distance_estimator import StepBasedDistanceEstimator
from .evaluator import Prediction, RunResult, aggregate
from .filters import butterworth_hp, butterworth_lp
from .motion_gate import MotionGate
from .roughness import inter_peak_rms_g
from .step_detector import Peak, StepDetector


def process(run) -> dict:
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


def predict_with_coeffs(coeffs: np.ndarray, peaks: list[Peak]) -> float:
    """V1 single-feature: stride = c0 + c1·amp.
    distance = c0·N + c1·Σamp."""
    if len(peaks) == 0:
        return max(float(coeffs[0]) * 0 + 0, 0.0)
    sum_amp = sum(p.amplitude_g for p in peaks)
    return max(float(coeffs[0] * len(peaks) + coeffs[1] * sum_amp), 0.0)


def fit_surface_coeffs(walks_with_peaks: list[tuple[list[Peak], float]]
                        ) -> np.ndarray:
    """Returns [c0, c1] for the V1 single-feature model."""
    rows = []
    y = []
    for peaks, distance in walks_with_peaks:
        sum_amp = sum(p.amplitude_g for p in peaks)
        rows.append([float(len(peaks)), float(sum_amp)])
        y.append(distance)
    X = np.array(rows, dtype=np.float64)
    y = np.array(y, dtype=np.float64)
    A = X.T @ X + 1e-3 * np.eye(2)
    return np.linalg.solve(A, X.T @ y)


def hard_threshold_pick(R: float, threshold: float,
                         indoor_coeffs: np.ndarray,
                         outdoor_coeffs: np.ndarray) -> np.ndarray:
    return indoor_coeffs if R < threshold else outdoor_coeffs


def soft_blend_pick(R: float, threshold: float, scale: float,
                     indoor_coeffs: np.ndarray,
                     outdoor_coeffs: np.ndarray) -> np.ndarray:
    """Sigmoid-weighted blend. scale controls sharpness."""
    if scale <= 0:
        return hard_threshold_pick(R, threshold, indoor_coeffs, outdoor_coeffs)
    w_outdoor = 1.0 / (1.0 + np.exp(-(R - threshold) / scale))
    return (1 - w_outdoor) * indoor_coeffs + w_outdoor * outdoor_coeffs


def loocv_auto_surface(indoor_p: list[dict], outdoor_p: list[dict],
                        classifier, classifier_name: str
                        ) -> tuple[list[RunResult], list[str]]:
    """LOO classifier. Returns (per-fold RunResults, surface tags)."""
    pooled = indoor_p + outdoor_p
    n_in = len(indoor_p)
    surface_tags = ["indoor"] * len(indoor_p) + ["outdoor"] * len(outdoor_p)
    per_fold: list[RunResult] = []
    classifier_decisions: list[str] = []

    for i, held_out in enumerate(pooled):
        held_surface = surface_tags[i]
        # Train indoor coefficients on all OTHER indoor walks
        indoor_train = [(d["peaks"], d["run"].actual_distance_ft)
                         for j, d in enumerate(indoor_p)
                         if (held_surface == "outdoor" or j != i)]
        outdoor_train = [(d["peaks"], d["run"].actual_distance_ft)
                          for j, d in enumerate(outdoor_p)
                          if (held_surface == "indoor"
                              or j != i - n_in)]
        indoor_coeffs = fit_surface_coeffs(indoor_train)
        outdoor_coeffs = fit_surface_coeffs(outdoor_train)
        # Classify held-out walk by its R
        R = held_out["R"]
        chosen = classifier(R, indoor_coeffs, outdoor_coeffs)
        # Decision label for diagnostics
        if np.allclose(chosen, indoor_coeffs):
            decision = "→indoor"
        elif np.allclose(chosen, outdoor_coeffs):
            decision = "→outdoor"
        else:
            # Soft blend
            w_out = ((chosen[1] - indoor_coeffs[1])
                     / (outdoor_coeffs[1] - indoor_coeffs[1] + 1e-12))
            decision = f"blend(w_out={w_out:.2f})"
        classifier_decisions.append(decision)
        pred = predict_with_coeffs(chosen, held_out["peaks"])
        per_fold.append(RunResult(
            run=held_out["run"],
            pred=Prediction(distance_ft=pred,
                            step_count=len(held_out["peaks"]),
                            diagnostics={"R": R,
                                         "classifier": decision}),
        ))
    return per_fold, classifier_decisions


def main() -> int:
    indoor_runs = load_capture_day("raw_sessions/2026-04-23")
    outdoor_runs = load_capture_day("raw_sessions/2026-04-25")
    indoor_walks = [r for r in iter_walks(indoor_runs) if r.valid]
    outdoor_walks = [r for r in iter_walks(outdoor_runs) if r.valid]
    indoor_p = [process(r) for r in indoor_walks]
    outdoor_p = [process(r) for r in outdoor_walks]

    print("=" * 80)
    print("R values per walk (motion-gated inter-peak RMS)")
    print("=" * 80)
    for d in indoor_p:
        print(f"  indoor   run {d['run'].run_idx:>2}  R = {d['R']:.4f}")
    for d in outdoor_p:
        print(f"  outdoor  run {d['run'].run_idx:>2}  R = {d['R']:.4f}")
    print()
    in_r = sorted(d["R"] for d in indoor_p)
    out_r = sorted(d["R"] for d in outdoor_p)
    print(f"  indoor sorted:  {[f'{r:.3f}' for r in in_r]}")
    print(f"  outdoor sorted: {[f'{r:.3f}' for r in out_r]}")
    print(f"  midpoint (in_max + out_min)/2 = "
          f"{(max(in_r) + min(out_r))/2:.4f}")
    print(f"  midpoint (in_med + out_med)/2 = "
          f"{(np.median(in_r) + np.median(out_r))/2:.4f}")
    print()

    # ------------------------------------------------------------------
    # Compare classifier configurations
    # ------------------------------------------------------------------
    surface_tags = (["indoor"] * len(indoor_p)
                    + ["outdoor"] * len(outdoor_p))

    configs = [
        ("hard τ=0.225 (in_max ≈ out_min boundary)",
         lambda R, i, o: hard_threshold_pick(R, 0.225, i, o)),
        ("hard τ=0.245 (median midpoint)",
         lambda R, i, o: hard_threshold_pick(R, 0.245, i, o)),
        ("hard τ=0.270",
         lambda R, i, o: hard_threshold_pick(R, 0.270, i, o)),
        ("soft τ=0.245 scale=0.020",
         lambda R, i, o: soft_blend_pick(R, 0.245, 0.020, i, o)),
        ("soft τ=0.245 scale=0.040",
         lambda R, i, o: soft_blend_pick(R, 0.245, 0.040, i, o)),
        ("soft τ=0.245 scale=0.060",
         lambda R, i, o: soft_blend_pick(R, 0.245, 0.060, i, o)),
    ]

    summary_rows = []
    for label, classifier in configs:
        per_fold, decisions = loocv_auto_surface(indoor_p, outdoor_p,
                                                  classifier, label)
        # Confusion: for hard classifiers, compare classifier output
        # vs ground-truth surface
        if "hard" in label:
            misclassified = []
            for tag, dec, rr in zip(surface_tags, decisions, per_fold):
                expected = "→" + tag
                if dec != expected:
                    misclassified.append(
                        (tag, rr.run.run_idx,
                         rr.pred.diagnostics["R"], dec))
            mis_str = (f"  misclassified: {len(misclassified)}/16"
                       + (f" ({', '.join(f'{t} run {idx}' for t,idx,_,_ in misclassified)})"
                          if misclassified else ""))
        else:
            mis_str = ""

        agg_in = aggregate([rr for tag, rr in zip(surface_tags, per_fold)
                             if tag == "indoor"])
        agg_out = aggregate([rr for tag, rr in zip(surface_tags, per_fold)
                              if tag == "outdoor"])
        agg_pooled = aggregate(per_fold)
        print("=" * 80)
        print(f"AUTO-SURFACE: {label}")
        print("=" * 80)
        print(f"  {'surf':<8} {'run':>3} {'truth':>5} {'pred':>6} "
              f"{'err%':>6}  {'R':>6}  {'decision'}")
        for tag, dec, rr in zip(surface_tags, decisions, per_fold):
            err = (f"{100*rr.pct_err:+.1f}"
                   if np.isfinite(rr.pct_err) else "-")
            R = rr.pred.diagnostics["R"]
            print(f"  {tag:<8} {rr.run.run_idx:>3} {rr.truth_ft:>5.1f} "
                  f"{rr.pred.distance_ft:>6.2f} {err:>6}  "
                  f"{R:>6.3f}  {dec}")
        print(f"  → indoor  MAPE {100*agg_in.distance_mape:.1f}%  "
              f"outdoor MAPE {100*agg_out.distance_mape:.1f}%  "
              f"pooled MAPE {100*agg_pooled.distance_mape:.1f}%")
        if mis_str:
            print(mis_str)
        print()
        summary_rows.append((label,
                              100*agg_in.distance_mape,
                              100*agg_out.distance_mape,
                              100*agg_pooled.distance_mape))

    # Final summary
    print("=" * 80)
    print("SUMMARY — auto-surface classifier vs operator-supplied surface")
    print("=" * 80)
    print(f"  {'config':<55}  {'in MAPE':>8}  {'out MAPE':>9}  {'pooled':>7}")
    print(f"  {'BASELINE: per-surface (operator picks the surface)':<55}  "
          f"{'16.4%':>8}  {'23.3%':>9}  {'—':>7}")
    for label, im, om, pm in summary_rows:
        short = ("auto: " + label)[:55].ljust(55)
        print(f"  {short}  {im:>7.1f}%  {om:>8.1f}%  {pm:>6.1f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

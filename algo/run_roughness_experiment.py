"""Surface-roughness regression experiment.

Hypothesis: a session-level roughness metric, computed from the IMU
stream itself, lets a SINGLE coefficient set carry across surfaces —
beating both the per-surface table (which doesn't scale to the wild)
and the multi-feature combined regression (which we already saw fails
without surface info).

Procedure:

  1. Compute both candidate roughness metrics for all 16 walks.
  2. Plot indoor vs outdoor distributions — does either metric
     cleanly separate surfaces?
  3. Fit a roughness-modulated regression
        stride = a + (b + d·R)·amp
     ⇒ distance = a·N + b·Σamp + d·R·Σamp
     where R is the per-session roughness. 3 features per row,
     ridge-regularized.
  4. LOO-evaluate, compare to:
       - V1 single-feature per-surface (current proposal)
       - Multi-feature amp+duration combined (Path D)
       - Single-feature amp combined

Output: algo/figures/roughness_distributions.png + numeric summary.
"""

from __future__ import annotations

from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

from .data_loader import iter_walks, load_capture_day
from .distance_estimator import StepBasedDistanceEstimator
from .evaluator import RunResult, aggregate
from .filters import butterworth_hp, butterworth_lp
from .motion_gate import MotionGate
from .roughness import hf_lf_ratio, inter_peak_rms_g
from .step_detector import Peak, StepDetector


FIG_DIR = Path(__file__).parent / "figures"


# ---------------------------------------------------------------------
# Compute roughness + peaks per run, in one pass over the same pipeline
# the V1 estimator uses.
# ---------------------------------------------------------------------


def process_run(run) -> dict:
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

    peak_idxs = [p.sample_idx for p in peaks]
    return {
        "run": run,
        "fs": fs,
        "peaks": peaks,
        "ipr": inter_peak_rms_g(mag_lp, peak_idxs, fs,
                                motion_mask=motion_mask),
        "hflf": hf_lf_ratio(mag_hp, fs, motion_mask=motion_mask),
        "motion_duration_s": gate.motion_duration_s,
    }


# ---------------------------------------------------------------------
# Roughness-modulated regression.
#
# stride_i = a + (b + d·R)·amp_i
# distance = a·N + b·Σamp + d·R·Σamp
#
# Per-run row: [N, Σamp, R·Σamp]; intercept absorbed into a.
# 3 features, intercept on, ridge α=1e-3.
# ---------------------------------------------------------------------


def fit_roughness_model(processed: list[dict], r_key: str = "hflf",
                        alpha: float = 1e-3) -> np.ndarray:
    """Per-peak model: stride_i = b + (c + d·R)·amp_i
    Per-run aggregate: distance = b·N + c·Σamp + d·R·Σamp
    (No constant intercept — that doesn't sum properly across peaks.)
    `r_key` selects the roughness metric: 'hflf' (HF/LF PSD ratio,
    captures 5-30 Hz texture energy explicitly) or 'ipr' (inter-peak
    RMS on smoothed signal — measures gait fluctuations, not texture)."""
    rows = []
    y = []
    for d in processed:
        peaks = d["peaks"]
        n_peaks = len(peaks)
        sum_amp = sum(p.amplitude_g for p in peaks)
        R = d[r_key]
        rows.append([n_peaks, sum_amp, R * sum_amp])
        y.append(d["run"].actual_distance_ft)
    X = np.array(rows, dtype=np.float64)
    y = np.array(y, dtype=np.float64)
    A = X.T @ X + alpha * np.eye(X.shape[1])
    return np.linalg.solve(A, X.T @ y)


def predict_with_roughness(coeffs: np.ndarray, d: dict,
                            r_key: str = "hflf") -> float:
    n_peaks = len(d["peaks"])
    sum_amp = sum(p.amplitude_g for p in d["peaks"])
    R = d[r_key]
    x = np.array([n_peaks, sum_amp, R * sum_amp])
    return float(x @ coeffs)


def loocv_roughness(processed: list[dict]) -> tuple[list[RunResult], np.ndarray]:
    per_fold = []
    for i, held_out in enumerate(processed):
        train = [d for j, d in enumerate(processed) if j != i]
        coeffs = fit_roughness_model(train)
        pred = max(predict_with_roughness(coeffs, held_out), 0.0)
        from .evaluator import Prediction
        per_fold.append(RunResult(
            run=held_out["run"],
            pred=Prediction(distance_ft=pred,
                            step_count=len(held_out["peaks"]),
                            diagnostics={"ipr": held_out["ipr"],
                                         "hflf": held_out["hflf"]}),
        ))
    final_coeffs = fit_roughness_model(processed)
    return per_fold, final_coeffs


# ---------------------------------------------------------------------
# Plot
# ---------------------------------------------------------------------


def plot_roughness_distributions(indoor: list[dict], outdoor: list[dict],
                                  out: Path) -> None:
    fig, axes = plt.subplots(1, 3, figsize=(15, 4))

    # (1) Inter-peak RMS comparison
    ax = axes[0]
    in_ipr = [d["ipr"] for d in indoor if np.isfinite(d["ipr"])]
    out_ipr = [d["ipr"] for d in outdoor if np.isfinite(d["ipr"])]
    bp = ax.boxplot([in_ipr, out_ipr], tick_labels=["indoor", "outdoor"],
                    widths=0.5, patch_artist=True)
    bp["boxes"][0].set_facecolor("steelblue")
    bp["boxes"][1].set_facecolor("darkorange")
    for i, vals in enumerate([in_ipr, out_ipr]):
        ax.scatter(np.full(len(vals), i + 1), vals,
                   color="black", alpha=0.6, s=30, zorder=3)
    ax.set_ylabel("inter-peak RMS [g]")
    ax.set_title("Metric 1: inter-peak RMS")
    ax.grid(alpha=0.3, axis="y")

    # (2) HF/LF ratio comparison
    ax = axes[1]
    in_hl = [d["hflf"] for d in indoor if np.isfinite(d["hflf"])]
    out_hl = [d["hflf"] for d in outdoor if np.isfinite(d["hflf"])]
    bp = ax.boxplot([in_hl, out_hl], tick_labels=["indoor", "outdoor"],
                    widths=0.5, patch_artist=True)
    bp["boxes"][0].set_facecolor("steelblue")
    bp["boxes"][1].set_facecolor("darkorange")
    for i, vals in enumerate([in_hl, out_hl]):
        ax.scatter(np.full(len(vals), i + 1), vals,
                   color="black", alpha=0.6, s=30, zorder=3)
    ax.set_ylabel("HF/LF PSD ratio")
    ax.set_title("Metric 2: HF/LF PSD ratio")
    ax.grid(alpha=0.3, axis="y")

    # (3) IPR vs sum_amp scatter, colored by surface
    ax = axes[2]
    for surface, data, color in (("indoor", indoor, "steelblue"),
                                  ("outdoor", outdoor, "darkorange")):
        for d in data:
            sum_amp = sum(p.amplitude_g for p in d["peaks"])
            ax.scatter(d["ipr"], sum_amp, color=color, alpha=0.7, s=40,
                       label=surface if d is data[0] else None)
            ax.annotate(f"{d['run'].run_idx}",
                        (d["ipr"], sum_amp),
                        fontsize=7, alpha=0.7,
                        xytext=(3, 3), textcoords="offset points")
    ax.set_xlabel("inter-peak RMS [g]  (roughness)")
    ax.set_ylabel("Σ peak amp [g]")
    ax.set_title("Roughness vs total peak amplitude")
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)

    fig.suptitle("Surface-roughness metrics — separation between "
                 "indoor and outdoor walks (n=8 each)", fontsize=10)
    fig.tight_layout()
    fig.savefig(out, dpi=140, bbox_inches="tight")
    plt.close(fig)


# ---------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------


def main() -> int:
    indoor_runs = load_capture_day("raw_sessions/2026-04-23")
    outdoor_runs = load_capture_day("raw_sessions/2026-04-25")
    indoor_walks = [r for r in iter_walks(indoor_runs) if r.valid]
    outdoor_walks = [r for r in iter_walks(outdoor_runs) if r.valid]

    indoor_p = [process_run(r) for r in indoor_walks]
    outdoor_p = [process_run(r) for r in outdoor_walks]

    print("=" * 80)
    print("ROUGHNESS METRICS — per-session values")
    print("=" * 80)
    print(f"  {'surface':<8}  {'run':>3}  {'speed':<6}  "
          f"{'inter_peak_rms':>14}  {'hf_lf_ratio':>12}  "
          f"{'Σamp':>7}  {'N_peaks':>7}")
    for surface, data in (("indoor", indoor_p), ("outdoor", outdoor_p)):
        for d in data:
            sum_amp = sum(p.amplitude_g for p in d["peaks"])
            print(f"  {surface:<8}  {d['run'].run_idx:>3}  "
                  f"{d['run'].intended_speed:<6}  "
                  f"{d['ipr']:>14.4f}  {d['hflf']:>12.4f}  "
                  f"{sum_amp:>7.2f}  {len(d['peaks']):>7}")
    in_ipr = [d["ipr"] for d in indoor_p]
    out_ipr = [d["ipr"] for d in outdoor_p]
    in_hl = [d["hflf"] for d in indoor_p]
    out_hl = [d["hflf"] for d in outdoor_p]
    print()
    print(f"  inter-peak RMS:  indoor median {np.median(in_ipr):.4f} g, "
          f"outdoor median {np.median(out_ipr):.4f} g, "
          f"ratio {np.median(out_ipr)/np.median(in_ipr):.2f}×")
    print(f"  HF/LF ratio:     indoor median {np.median(in_hl):.4f}, "
          f"outdoor median {np.median(out_hl):.4f}, "
          f"ratio {np.median(out_hl)/np.median(in_hl):.2f}×")
    print(f"  separation IPR:   max(indoor)={max(in_ipr):.4f}, "
          f"min(outdoor)={min(out_ipr):.4f} → "
          f"{'CLEAN' if max(in_ipr) < min(out_ipr) else 'OVERLAP'}")
    print(f"  separation HF/LF: max(indoor)={max(in_hl):.4f}, "
          f"min(outdoor)={min(out_hl):.4f} → "
          f"{'CLEAN' if max(in_hl) < min(out_hl) else 'OVERLAP'}")

    plot_roughness_distributions(indoor_p, outdoor_p,
                                  FIG_DIR / "roughness_distributions.png")
    print()
    print(f"Figure: {FIG_DIR / 'roughness_distributions.png'}")

    # ------------------------------------------------------------------
    # Roughness-modulated combined LOO
    # ------------------------------------------------------------------
    print()
    print("=" * 80)
    print("ROUGHNESS-MODULATED COMBINED LOO  (16 walks, single coefficient set)")
    print("=" * 80)
    print("  Model: stride_i = b + (c + d·R)·amp_i")
    print("         distance = b·N + c·Σamp + d·R·Σamp")
    pooled = indoor_p + outdoor_p
    surface_tag = ["indoor"] * len(indoor_p) + ["outdoor"] * len(outdoor_p)
    # Run BOTH metrics for comparison.
    print()
    summary_rows = []
    for r_key, label in (("ipr", "inter-peak RMS (LP-smoothed channel)"),
                          ("hflf", "HF/LF PSD ratio (un-smoothed |a|_HP)")):
        per_fold = []
        for i, held_out in enumerate(pooled):
            train = [d for j, d in enumerate(pooled) if j != i]
            coeffs = fit_roughness_model(train, r_key=r_key)
            pred = max(predict_with_roughness(coeffs, held_out, r_key=r_key), 0.0)
            from .evaluator import Prediction
            per_fold.append(RunResult(
                run=held_out["run"],
                pred=Prediction(distance_ft=pred,
                                step_count=len(held_out["peaks"]),
                                diagnostics={"R": held_out[r_key]}),
            ))
        agg = aggregate(per_fold)
        agg_in = aggregate([rr for tag, rr in zip(surface_tag, per_fold)
                             if tag == "indoor"])
        agg_out = aggregate([rr for tag, rr in zip(surface_tag, per_fold)
                              if tag == "outdoor"])
        final = fit_roughness_model(pooled, r_key=r_key)
        print(f"  R = {label}")
        print(f"    indoor MAPE  {100*agg_in.distance_mape:5.1f}%   "
              f"outdoor MAPE {100*agg_out.distance_mape:5.1f}%   "
              f"pooled {100*agg.distance_mape:5.1f}%")
        # Effective slopes
        R_in_med = np.median([d[r_key] for d in indoor_p
                               if np.isfinite(d[r_key])])
        R_out_med = np.median([d[r_key] for d in outdoor_p
                                if np.isfinite(d[r_key])])
        slope_in = final[1] + final[2] * R_in_med
        slope_out = final[1] + final[2] * R_out_med
        print(f"    coeffs: b={final[0]:+.3f} ft/peak, "
              f"c={final[1]:+.3f} ft/g, d={final[2]:+.3f}")
        print(f"    effective slope at R_indoor_med={R_in_med:.3f}: "
              f"{slope_in:.3f}  (per-surface fit: 2.757)")
        print(f"    effective slope at R_outdoor_med={R_out_med:.3f}: "
              f"{slope_out:.3f}  (per-surface fit: 1.705)")
        print()
        summary_rows.append((label, 100*agg_in.distance_mape,
                              100*agg_out.distance_mape,
                              100*agg.distance_mape))
    # ------------------------------------------------------------------
    # Side-by-side comparison vs prior approaches
    # ------------------------------------------------------------------
    print("=" * 80)
    print("SUMMARY — winner picks the v1 architecture")
    print("=" * 80)
    print(f"  {'approach':<60}  {'in MAPE':>8}  {'out MAPE':>9}  {'pooled':>7}")
    print(f"  {'A: per-surface single feature (hardcoded table)':<60}  "
          f"{'16.4%':>8}  {'23.3%':>9}  {'—':>7}")
    print(f"  {'D: amp+duration combined (no surface info)':<60}  "
          f"{'20.3%':>8}  {'42.0%':>9}  {'31.2%':>7}")
    print(f"  {'D: amp combined (no surface info)':<60}  "
          f"{'26.9%':>8}  {'32.4%':>9}  {'29.6%':>7}")
    for label, in_m, out_m, pooled_m in summary_rows:
        short = ("E: roughness · " + label[:42]).ljust(60)
        print(f"  {short}  {in_m:>7.1f}%  {out_m:>8.1f}%  {pooled_m:>6.1f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

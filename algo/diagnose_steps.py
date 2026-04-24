"""Are we really seeing per-step impulses, or are we imposing false
discretization on a smoothly-rolling signal?

Three tests, all on the same per-run data:

  1. STRICT vs LOOSE peak detection. My streaming Schmitt trigger uses
     enter=0.05 g. scipy.find_peaks with prominence=0.005 g is ~10× more
     sensitive. If the loose detector finds approximately `manual_step_count`
     peaks, the impulse structure is real and my streaming detector is
     just too conservative. If the loose detector finds way more peaks
     than manual, it's finding noise rather than steps. If it still
     finds fewer than manual, the manual count reflects something not
     visible in accel.

  2. AUTOCORRELATION of |a|_HP_LP. Rhythmic gait → autocorrelation has
     clean side-lobes at the period of the gait. Smooth glide → decays
     monotonically to zero with no side-lobe structure.

  3. VISUAL OVERLAY on 4 runs: manual step count (as expected inter-step
     interval) overlaid on the actual signal + both detectors' peaks.

Output: algo/figures/steps_diagnostic.png — six-panel diagnostic.
"""

from __future__ import annotations

from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from scipy import signal

from .data_loader import Run, load_capture_day
from .distance_estimator import StepBasedDistanceEstimator
from .filters import butterworth_hp, butterworth_lp

FIG_DIR = Path(__file__).parent / "figures"


def _pipeline(run: Run, hp_hz=0.2, lp_hz=5.0):
    """Replicate the streaming pipeline offline so we can inspect
    the intermediate signals."""
    fs = run.derived_rate_hz
    hp = butterworth_hp(hp_hz, fs=fs)
    lp = butterworth_lp(lp_hz, fs=fs)
    hp.init_steady(float(run.accel_mag_g[0]))
    mag_g = run.accel_mag_g
    mag_hp = np.empty_like(mag_g)
    mag_lp = np.empty_like(mag_g)
    for i, x in enumerate(mag_g):
        v = hp.step(x - 1.0)
        mag_hp[i] = v
        mag_lp[i] = lp.step(v)
    t = np.arange(len(mag_g)) / fs
    return t, mag_hp, mag_lp, fs


def _strict_peaks(run: Run) -> list[int]:
    """Sample indices from my streaming detector."""
    est = StepBasedDistanceEstimator()
    res = est._process_run(run)
    return [p.sample_idx for p in res["peaks"]]


def _loose_peaks(mag_lp: np.ndarray, fs: float,
                 prominence=0.02, distance_s=0.5) -> np.ndarray:
    """scipy find_peaks with low prominence + short min-distance —
    'find every bump that could plausibly be a step'."""
    peaks, _ = signal.find_peaks(
        mag_lp,
        prominence=prominence,
        distance=int(fs * distance_s),
    )
    return peaks


def main() -> int:
    runs = load_capture_day("raw_sessions/2026-04-23")
    walks = [r for r in runs if r.is_walk]

    print("=== Strict (streaming) vs. loose (scipy, low-prominence) peaks ===")
    print(f"{'run':>3}  {'manual':>6}  {'strict':>6}  "
          f"{'loose_0.02g':>11}  {'loose_0.01g':>11}  {'loose_0.005g':>12}  "
          f"{'note':<30}")
    print("-" * 90)

    per_run_rows = []
    for r in walks:
        t, mag_hp, mag_lp, fs = _pipeline(r)
        strict = _strict_peaks(r)
        loose_02 = _loose_peaks(mag_lp, fs, prominence=0.02)
        loose_01 = _loose_peaks(mag_lp, fs, prominence=0.01)
        loose_005 = _loose_peaks(mag_lp, fs, prominence=0.005)

        manual = int(r.manual_step_count) if np.isfinite(r.manual_step_count) else None
        note = ""
        if manual is not None:
            best_loose = min(
                [("0.02", loose_02), ("0.01", loose_01), ("0.005", loose_005)],
                key=lambda kv: abs(len(kv[1]) - manual),
            )
            note = f"closest to manual: prom {best_loose[0]}g → {len(best_loose[1])}"

        per_run_rows.append(dict(
            run=r, t=t, mag_hp=mag_hp, mag_lp=mag_lp, fs=fs,
            strict=strict, loose_02=loose_02, loose_01=loose_01,
            loose_005=loose_005, manual=manual,
        ))
        print(f"{r.run_idx:>3}  "
              f"{'-' if manual is None else str(manual):>6}  "
              f"{len(strict):>6}  "
              f"{len(loose_02):>11}  {len(loose_01):>11}  {len(loose_005):>12}  "
              f"{note:<30}")

    # Visual: pick 4 runs including the worst miss (run 5) + a clean one
    # (run 1) + the curve (run 8) + run 3 for direct comparison with
    # characterization.py output.
    target_idxs = [1, 3, 5, 8]
    selected = [row for row in per_run_rows if row["run"].run_idx in target_idxs]

    fig, axes = plt.subplots(len(selected) + 1, 1,
                             figsize=(14, 2.5 * (len(selected) + 1)))
    for ax, row in zip(axes[:len(selected)], selected):
        r = row["run"]
        t = row["t"]
        mag_lp = row["mag_lp"]
        mag_hp = row["mag_hp"]
        strict = row["strict"]
        loose = row["loose_01"]
        manual = row["manual"]

        ax.plot(t, mag_hp, color="lightsteelblue", lw=0.4, alpha=0.6,
                label="|a|_HP")
        ax.plot(t, mag_lp, color="navy", lw=0.8,
                label="|a|_HP LP 5Hz")
        ax.plot(t[strict], mag_lp[strict], "ro", ms=10, mfc="none",
                mew=1.5, label=f"strict (streaming) — {len(strict)} peaks")
        ax.plot(t[loose], mag_lp[loose], "g+", ms=12, mew=1.5,
                label=f"loose (prom 0.01g) — {len(loose)} peaks")

        title = (f"Run {r.run_idx} — {r.run_type}/{r.direction}, "
                 f"{r.actual_distance_ft:.0f} ft")
        if manual is not None:
            title += f", manual = {manual} steps"
        ax.set_title(title, fontsize=10)
        ax.set_ylabel("|a|_HP [g]")
        ax.axhline(0.05, color="red", lw=0.5, ls=":", alpha=0.5,
                   label="strict enter=0.05g")
        ax.axhline(0.01, color="green", lw=0.5, ls=":", alpha=0.5,
                   label="loose prom=0.01g")
        ax.grid(alpha=0.3)
        ax.legend(fontsize=7, loc="upper right", ncol=3)

    # Autocorrelation panel: average across the 4 selected walks.
    ax_ac = axes[-1]
    for row in selected:
        r = row["run"]
        mag = row["mag_lp"]
        # Trim to motion region (roughly).
        active = mag > 0.02
        first = np.argmax(active)
        last = len(mag) - np.argmax(active[::-1])
        seg = mag[first:last]
        seg = seg - seg.mean()
        if len(seg) < 200:
            continue
        ac = np.correlate(seg, seg, mode="full")
        ac = ac[len(ac) // 2:] / ac[len(ac) // 2]
        lag_s = np.arange(len(ac)) / row["fs"]
        mask = lag_s < 5.0
        ax_ac.plot(lag_s[mask], ac[mask], lw=0.9,
                   label=f"run {r.run_idx} ({row['manual']} steps)"
                   if row['manual'] else f"run {r.run_idx}")
    ax_ac.axhline(0, color="gray", lw=0.5)
    ax_ac.set_xlabel("lag [s]")
    ax_ac.set_ylabel("autocorrelation of |a|_HP_LP")
    ax_ac.set_title("Autocorrelation — side-lobes at gait period ⇒ rhythmic; "
                    "monotone decay ⇒ smooth glide")
    ax_ac.grid(alpha=0.3)
    ax_ac.legend(fontsize=8)

    fig.tight_layout()
    out = FIG_DIR / "steps_diagnostic.png"
    fig.savefig(out, dpi=140, bbox_inches="tight")
    plt.close(fig)
    print(f"\nFigure: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

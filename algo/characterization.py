"""Phase 2 — gait characterization on the 2026-04-23 shakedown dataset.

Produces plots + numeric summaries needed to pick filter cutoffs and
decide whether step-based detection or continuous-glide integration is
the right primary model for the two-wheel/glide walker.

Outputs land in algo/figures/:
  - per_run_timeseries.png    grid of |a| and |ω| for every run
  - stationary_noise_floor.png  run 9 expanded, noise-floor histogram
  - psd_overlay.png           PSD of |a| across walking runs + stationary
  - candidate_peaks.png       provisional peak detection on run 3 (normal)

A text summary is also printed — filter-cutoff proposal, signal-shape
verdict, noise floor numbers.

Run from the repo root:
    algo/venv/bin/python3 -m algo.characterization
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from scipy import signal

from .data_loader import Run, load_capture_day

FIG_DIR = Path(__file__).parent / "figures"
FIG_DIR.mkdir(exist_ok=True)


# ---------------------------------------------------------------------
# Per-run numeric summary
# ---------------------------------------------------------------------


@dataclass
class RunStats:
    run: Run
    mean_a_g: float         # mean |a| in g — should be ~1.0
    std_a_g: float          # std |a| in g
    std_a_hp_g: float       # std after gravity HP filter — gait signal strength
    dom_freq_hz: float      # dominant frequency of |a|_hp PSD
    dom_power: float        # relative power at dom_freq (vs. full-band)
    est_cadence_hz: float   # manual steps / duration, NaN if no step count


def _gravity_removed_mag(run: Run, fs: float, hp_hz: float = 0.2) -> np.ndarray:
    """|a| with gravity removed via a 2nd-order Butterworth HP. Returned
    in g. Zero-phase filtering (filtfilt) is used for characterization;
    the production streaming version will use the causal form of the
    same filter."""
    sos = signal.butter(2, hp_hz, btype="high", fs=fs, output="sos")
    mag_g = run.accel_mag_g
    return signal.sosfiltfilt(sos, mag_g - mag_g.mean())


def _psd(x: np.ndarray, fs: float) -> tuple[np.ndarray, np.ndarray]:
    """Welch PSD. Segment = 4 s (400 samples @ 100 Hz), Hann, 50% overlap."""
    nperseg = min(400, len(x))
    return signal.welch(x, fs=fs, nperseg=nperseg, noverlap=nperseg // 2,
                        window="hann", scaling="density")


def summarize_run(run: Run, noise_band_hz: tuple[float, float] = (0.3, 3.0)) -> RunStats:
    fs = run.derived_rate_hz
    mag_g = run.accel_mag_g
    mag_hp = _gravity_removed_mag(run, fs)
    f, pxx = _psd(mag_hp, fs)
    band = (f >= noise_band_hz[0]) & (f <= noise_band_hz[1])
    band_idx = np.where(band)[0]
    if len(band_idx) and pxx[band_idx].sum() > 0:
        peak_idx = band_idx[np.argmax(pxx[band_idx])]
        dom_freq = float(f[peak_idx])
        dom_power = float(pxx[peak_idx] / pxx.sum())
    else:
        dom_freq = float("nan")
        dom_power = float("nan")

    cadence = (run.manual_step_count / (run.n_samples / fs)
               if np.isfinite(run.manual_step_count) else float("nan"))

    return RunStats(
        run=run,
        mean_a_g=float(mag_g.mean()),
        std_a_g=float(mag_g.std()),
        std_a_hp_g=float(mag_hp.std()),
        dom_freq_hz=dom_freq,
        dom_power=dom_power,
        est_cadence_hz=cadence,
    )


# ---------------------------------------------------------------------
# Plots
# ---------------------------------------------------------------------


def plot_per_run_timeseries(runs: list[Run], out: Path) -> None:
    """Grid of |a| and |ω| time series, one row per run. Visual quick-
    look for: is there a rhythmic gait signal? how noisy is stationary?
    does stumble stand out?"""
    n = len(runs)
    fig, axes = plt.subplots(n, 2, figsize=(14, 1.6 * n), sharex="col")
    if n == 1:
        axes = np.array([axes])
    for i, r in enumerate(runs):
        fs = r.derived_rate_hz
        t = r.t_s - r.t_s[0]
        mag_a = r.accel_mag_g
        mag_g_hp = _gravity_removed_mag(r, fs)
        mag_w = np.linalg.norm(r.gyro_dps, axis=1)

        ax_l = axes[i, 0]
        ax_l.plot(t, mag_a, color="steelblue", lw=0.5, alpha=0.6, label="|a|")
        ax_l.plot(t, mag_g_hp + 1.0, color="crimson", lw=0.6,
                  label="|a|_HP (+1g offset)")
        ax_l.axhline(1.0, color="gray", lw=0.5, ls=":")
        ax_l.set_ylabel(f"run {r.run_idx}\n|a| [g]", fontsize=8)
        ax_l.set_ylim(0.5, 1.6)
        ax_l.tick_params(axis="both", labelsize=7)
        label = f"{r.run_type} / {r.direction} / {r.intended_speed}"
        label += f" / {r.actual_distance_ft:.0f} ft"
        if np.isfinite(r.manual_step_count):
            label += f" / {int(r.manual_step_count)} steps"
        ax_l.set_title(label, fontsize=8, loc="left")
        if i == 0:
            ax_l.legend(fontsize=7, loc="upper right")

        ax_r = axes[i, 1]
        ax_r.plot(t, mag_w, color="darkgreen", lw=0.5)
        ax_r.set_ylabel("|ω| [°/s]", fontsize=8)
        ax_r.tick_params(axis="both", labelsize=7)
        ax_r.set_ylim(0, max(30, 1.1 * np.percentile(mag_w, 99)))
    axes[-1, 0].set_xlabel("time [s]", fontsize=8)
    axes[-1, 1].set_xlabel("time [s]", fontsize=8)
    fig.suptitle("Per-run |a| (HP overlay in red) and |ω|  —  2026-04-23 shakedown",
                 fontsize=10)
    fig.tight_layout()
    fig.savefig(out, dpi=140, bbox_inches="tight")
    plt.close(fig)


def plot_stationary_noise_floor(stationary: Run, out: Path) -> None:
    """Run 9 expanded: |a| time series + HP-filtered histogram. Sets the
    bar for step-detection prominence threshold — any threshold below
    the stationary noise std will false-trigger while standing still."""
    fs = stationary.derived_rate_hz
    t = stationary.t_s - stationary.t_s[0]
    mag_g = stationary.accel_mag_g
    mag_hp = _gravity_removed_mag(stationary, fs)

    fig, axes = plt.subplots(1, 2, figsize=(12, 3.5))
    axes[0].plot(t, mag_g - 1.0, lw=0.5, color="steelblue",
                 label=r"$|a| - 1\,g$ (raw, mean-subtracted)")
    axes[0].plot(t, mag_hp, lw=0.6, color="crimson", label=r"$|a|_{\mathrm{HP}}$")
    axes[0].set_xlabel("time [s]")
    axes[0].set_ylabel("|a| residual [g]")
    axes[0].set_title(f"Run {stationary.run_idx} (stationary, {t[-1]:.1f} s)")
    axes[0].legend(fontsize=8)
    axes[0].grid(alpha=0.3)

    axes[1].hist(mag_hp, bins=50, color="crimson", alpha=0.8)
    axes[1].set_xlabel("|a|_HP [g]")
    axes[1].set_ylabel("count")
    axes[1].set_title(f"HP residual histogram  "
                      f"(σ = {mag_hp.std():.4f} g,  "
                      f"max = {np.abs(mag_hp).max():.4f} g)")
    axes[1].axvline(3 * mag_hp.std(), ls="--", color="k", lw=0.8,
                    label="±3σ")
    axes[1].axvline(-3 * mag_hp.std(), ls="--", color="k", lw=0.8)
    axes[1].legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out, dpi=140, bbox_inches="tight")
    plt.close(fig)


def plot_psd_overlay(runs: list[Run], out: Path) -> None:
    """PSD of |a|_HP for every run on one plot. Where does gait energy
    concentrate, and how does it sit vs. stationary noise?"""
    fig, ax = plt.subplots(figsize=(10, 5))
    for r in runs:
        fs = r.derived_rate_hz
        mag_hp = _gravity_removed_mag(r, fs)
        f, pxx = _psd(mag_hp, fs)
        if r.run_type == "stationary_baseline":
            color, ls, lw = "black", "--", 1.8
            label = f"run {r.run_idx} stationary"
        elif r.run_type == "stumble":
            color, ls, lw = "crimson", ":", 1.5
            label = f"run {r.run_idx} stumble"
        else:
            color, ls, lw = None, "-", 0.9
            label = (f"run {r.run_idx} {r.intended_speed} "
                     f"{r.actual_distance_ft:.0f}ft")
        ax.semilogy(f, pxx, label=label, color=color, ls=ls, lw=lw)
    ax.set_xlim(0, 6)
    ax.set_xlabel("frequency [Hz]")
    ax.set_ylabel("PSD of |a|_HP  [g²/Hz]")
    ax.set_title("Power spectrum across all 10 shakedown runs  "
                 "(after 0.2 Hz HP gravity-removal)")
    ax.grid(alpha=0.3, which="both")
    ax.legend(fontsize=7, ncol=2, loc="upper right")
    fig.tight_layout()
    fig.savefig(out, dpi=140, bbox_inches="tight")
    plt.close(fig)


def plot_candidate_peaks(
    run: Run,
    stationary_std_g: float,
    out: Path,
    lp_hz: float = 5.0,
) -> None:
    """Run a provisional peak detection on one walk and plot it.
    Prominence = 5× stationary noise σ, min distance = 1 s. Not the
    final algorithm — just "does step-detection look viable here?"."""
    fs = run.derived_rate_hz
    mag_hp = _gravity_removed_mag(run, fs)
    sos_lp = signal.butter(2, lp_hz, btype="low", fs=fs, output="sos")
    mag_smooth = signal.sosfiltfilt(sos_lp, mag_hp)
    t = run.t_s - run.t_s[0]

    prom = 5.0 * stationary_std_g
    min_dist = int(fs * 1.0)
    peaks, props = signal.find_peaks(mag_smooth, prominence=prom, distance=min_dist)

    fig, ax = plt.subplots(figsize=(12, 4))
    ax.plot(t, mag_hp, lw=0.4, color="steelblue", alpha=0.5, label="|a|_HP")
    ax.plot(t, mag_smooth, lw=0.8, color="navy", label=f"|a|_HP LP {lp_hz}Hz")
    ax.plot(t[peaks], mag_smooth[peaks], "rx", ms=10, mew=1.5,
            label=f"peaks (prominence≥{prom:.3f}g, min_dist 1s)")
    ax.axhline(prom, ls=":", color="gray", label=f"prominence threshold")
    manual = int(run.manual_step_count) if np.isfinite(run.manual_step_count) else "?"
    ax.set_title(f"Run {run.run_idx} ({run.intended_speed}) — "
                 f"{len(peaks)} peaks detected, {manual} manual steps")
    ax.set_xlabel("time [s]")
    ax.set_ylabel("|a|_HP [g]")
    ax.legend(fontsize=8, loc="upper right")
    ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(out, dpi=140, bbox_inches="tight")
    plt.close(fig)


# ---------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------


def _print_table(stats: list[RunStats]) -> None:
    header = (f"{'run':>3}  {'type':<20}  {'speed':<6}  {'dist':>5}  "
              f"{'steps':>5}  {'μ|a|':>6}  {'σ|a|':>6}  {'σ|a|HP':>7}  "
              f"{'f_dom':>6}  {'pwr%':>5}  {'cad':>5}")
    print(header)
    print("-" * len(header))
    for s in stats:
        steps = (f"{int(s.run.manual_step_count)}"
                 if np.isfinite(s.run.manual_step_count) else "-")
        cad = f"{s.est_cadence_hz:.2f}" if np.isfinite(s.est_cadence_hz) else "-"
        typ_dir = f"{s.run.run_type}/{s.run.direction}"
        print(f"{s.run.run_idx:>3}  {typ_dir:<20}  {s.run.intended_speed:<6}  "
              f"{s.run.actual_distance_ft:>5.1f}  {steps:>5}  "
              f"{s.mean_a_g:>6.3f}  {s.std_a_g:>6.3f}  {s.std_a_hp_g:>7.4f}  "
              f"{s.dom_freq_hz:>6.2f}  {100*s.dom_power:>5.1f}  {cad:>5}")


def main() -> int:
    runs = load_capture_day("raw_sessions/2026-04-23")
    stats = [summarize_run(r) for r in runs]

    print("=== Per-run characterization ===\n")
    _print_table(stats)

    stationary = next(r for r in runs if r.run_type == "stationary_baseline")
    stationary_stats = next(s for s in stats if s.run is stationary)

    walk_stats = [s for s in stats if s.run.is_walk]
    walk_dom = np.array([s.dom_freq_hz for s in walk_stats])
    walk_cad = np.array([s.est_cadence_hz for s in walk_stats
                         if np.isfinite(s.est_cadence_hz)])

    print()
    print("=== Summary ===")
    print(f"Stationary noise floor (σ of |a|_HP) = "
          f"{stationary_stats.std_a_hp_g:.4f} g")
    print(f"Stationary max excursion             = "
          f"{np.abs(_gravity_removed_mag(stationary, stationary.derived_rate_hz)).max():.4f} g")
    print(f"Walk |a|_HP σ (median)               = "
          f"{np.median([s.std_a_hp_g for s in walk_stats]):.3f} g  "
          f"(≈ {np.median([s.std_a_hp_g for s in walk_stats])/stationary_stats.std_a_hp_g:.0f}× stationary)")
    print(f"Dominant frequency of walks          = "
          f"{walk_dom.mean():.2f} ± {walk_dom.std():.2f} Hz  "
          f"(range {walk_dom.min():.2f}–{walk_dom.max():.2f})")
    if len(walk_cad) > 0:
        print(f"Manual cadence (steps/duration)      = "
              f"{walk_cad.mean():.2f} ± {walk_cad.std():.2f} Hz  "
              f"(range {walk_cad.min():.2f}–{walk_cad.max():.2f})")

    print()
    print("=== Figure outputs ===")
    plot_per_run_timeseries(runs, FIG_DIR / "per_run_timeseries.png")
    print(f"  {FIG_DIR/'per_run_timeseries.png'}")
    plot_stationary_noise_floor(stationary, FIG_DIR / "stationary_noise_floor.png")
    print(f"  {FIG_DIR/'stationary_noise_floor.png'}")
    plot_psd_overlay(runs, FIG_DIR / "psd_overlay.png")
    print(f"  {FIG_DIR/'psd_overlay.png'}")

    normal_run = next(r for r in runs
                      if r.run_idx == 3 and r.run_type == "normal")
    plot_candidate_peaks(normal_run, stationary_stats.std_a_hp_g,
                         FIG_DIR / "candidate_peaks.png")
    print(f"  {FIG_DIR/'candidate_peaks.png'}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

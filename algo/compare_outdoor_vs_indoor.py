"""Cross-surface signal characterization — outdoor concrete vs polished
concrete. Tests the hypothesis that the rougher outdoor surface puts
more energy into the cap and shifts the algorithm's operating point.

Compares on three axes:

  1. STATIONARY noise floor — outdoor (run 19, 50.4 s) vs indoor (run 9
     from 2026-04-23, 37.1 s). The Phase-2 indoor σ was 0.0009 g; if
     outdoor is meaningfully higher, the motion-gate's 0.01 g enter
     threshold (set at 11× indoor σ) needs revisiting.

  2. WALKING |a|_HP σ distribution — across all walks per surface.
     Indoor median was 0.307 g; if outdoor is higher (texture energy)
     the stride regression coefficients shift.

  3. PSD shape — outdoor walking should add HF content (5–30 Hz) from
     surface texture that indoor walks don't have. If yes, the LP at
     5 Hz is killing useful signal — or alternatively, helping by
     rejecting it.

Output: algo/figures/outdoor_vs_indoor.png + a numeric summary.
"""

from __future__ import annotations

from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from scipy import signal

from .data_loader import load_capture_day, parse_dat_file
from .filters import butterworth_hp


FIG_DIR = Path(__file__).parent / "figures"
G = 9.80665

# Outdoor concrete sessions (one per protocol slot; for slots with
# duplicates we take the post-fix capture).
OUTDOOR_BY_RUN = {
    "stationary":  ("2a47aeef-ce82-4359-8de9-b8b676966b53.dat", "stationary 50.4s"),
    "11/17 warmup": ("c029c6c3-26e1-46d0-b580-75130cf58c34.dat", "10ft normal 26.1s"),
    "12 slow":     ("3c6e2ba7-1250-411b-9122-d2112235fd28.dat", "20ft slow 38.2s"),
    "13 normal":   ("b1e7ef3b-e7fd-4afc-9567-9fe5e9a14740.dat", "20ft normal 30.4s"),
    "14 fast":     ("8fc08e9f-144e-48d3-9d60-2ff49001f0ea.dat", "20ft fast 24.4s"),
    "15 normal":   ("c87a2b8c-5b85-4e6c-afb7-9b74db514219.dat", "20ft normal 25.7s"),
    "16 long":     ("b09fe88b-4e2b-49b1-95c3-f86ed0bb125d.dat", "40ft normal 40.6s"),
    "17 short":    ("cf29f162-9fd1-4ffc-8ff2-0970c9f7ae9d.dat", "10ft normal 21.2s"),
    "20 pickup":   ("889d33a3-9573-4154-b483-dca893eb5a87.dat", "10ft pickup 30.5s"),
}


def load_one(path: Path):
    """Returns (mag_hp_g, fs)."""
    hdr, imu, _ = parse_dat_file(path)
    fs = (imu["t_ms"].max() - imu["t_ms"].min()) / (len(imu) - 1)
    fs_hz = 1000.0 / fs
    a = np.stack([imu["ax"], imu["ay"], imu["az"]], axis=1)
    mag_g = np.sqrt((a * a).sum(axis=1)) / G
    hp = butterworth_hp(0.2, fs=fs_hz)
    hp.init_steady(float(mag_g[0]) - 1.0)
    mag_hp = np.empty_like(mag_g)
    for i, x in enumerate(mag_g):
        mag_hp[i] = hp.step(x - 1.0)
    return mag_hp, fs_hz, hdr


def main() -> int:
    # Indoor
    indoor_runs = load_capture_day("raw_sessions/2026-04-23")
    indoor_walks = [r for r in indoor_runs if r.is_walk]
    indoor_stat = next(r for r in indoor_runs if r.run_type == "stationary_baseline")

    # Same gravity-removal pipeline for indoor
    def indoor_mag_hp(run):
        from .filters import butterworth_hp
        fs = run.derived_rate_hz
        mag = run.accel_mag_g
        hp = butterworth_hp(0.2, fs=fs)
        hp.init_steady(float(mag[0]) - 1.0)
        out = np.empty_like(mag)
        for i, x in enumerate(mag):
            out[i] = hp.step(x - 1.0)
        return out

    indoor_stat_hp = indoor_mag_hp(indoor_stat)
    indoor_walk_sigmas = []
    for r in indoor_walks:
        h = indoor_mag_hp(r)
        # σ over the middle half — avoid the start/stop transients
        n = len(h)
        seg = h[n // 4: 3 * n // 4]
        indoor_walk_sigmas.append(seg.std())

    # Outdoor — load via parse_dat_file since we have UUIDs not run_idx
    outdoor_data = {}
    for label, (fname, desc) in OUTDOOR_BY_RUN.items():
        mag_hp, fs, hdr = load_one(Path("raw_sessions/2026-04-25") / fname)
        outdoor_data[label] = dict(mag_hp=mag_hp, fs=fs, desc=desc, hdr=hdr)

    outdoor_stat_hp = outdoor_data["stationary"]["mag_hp"]

    outdoor_walk_sigmas = []
    for label, d in outdoor_data.items():
        if label == "stationary":
            continue
        n = len(d["mag_hp"])
        seg = d["mag_hp"][n // 4: 3 * n // 4]
        outdoor_walk_sigmas.append((label, d["desc"], seg.std()))

    # ---- Numeric summary ------------------------------------------
    print("=" * 80)
    print("STATIONARY noise floor (parked walker, gravity removed)")
    print("=" * 80)
    indoor_stat_sigma = indoor_stat_hp.std()
    indoor_stat_max = np.abs(indoor_stat_hp).max()
    outdoor_stat_sigma = outdoor_stat_hp.std()
    outdoor_stat_max = np.abs(outdoor_stat_hp).max()
    print(f"  indoor (polished concrete, run 9, 37 s): "
          f"σ = {indoor_stat_sigma*1000:.3f} mg, "
          f"max = {indoor_stat_max*1000:.2f} mg")
    print(f"  outdoor (sidewalk, run 19, 50 s):        "
          f"σ = {outdoor_stat_sigma*1000:.3f} mg, "
          f"max = {outdoor_stat_max*1000:.2f} mg")
    print(f"  ratio outdoor/indoor: σ {outdoor_stat_sigma/indoor_stat_sigma:.1f}×, "
          f"max {outdoor_stat_max/indoor_stat_max:.1f}×")

    print()
    print("=" * 80)
    print("WALKING signal σ (|a|_HP std over the middle half of each session)")
    print("=" * 80)
    print(f"  indoor walks (n={len(indoor_walks)}): "
          f"median = {np.median(indoor_walk_sigmas):.3f} g, "
          f"min = {min(indoor_walk_sigmas):.3f}, max = {max(indoor_walk_sigmas):.3f}")
    print(f"  outdoor walks (n={len(outdoor_walk_sigmas)}):")
    for label, desc, s in outdoor_walk_sigmas:
        print(f"    {label:<14} {desc:<20} σ = {s:.3f} g")
    outdoor_sigma_arr = np.array([s for _, _, s in outdoor_walk_sigmas])
    print(f"  outdoor median = {np.median(outdoor_sigma_arr):.3f} g, "
          f"min = {outdoor_sigma_arr.min():.3f}, max = {outdoor_sigma_arr.max():.3f}")
    print(f"  ratio outdoor/indoor median: "
          f"{np.median(outdoor_sigma_arr) / np.median(indoor_walk_sigmas):.2f}×")

    print()
    print("=" * 80)
    print("MOTION-GATE THRESHOLD CHECK (current enter=0.01g, exit=0.005g)")
    print("=" * 80)
    enter_g = 0.01
    enter_indoor_margin = enter_g / indoor_stat_sigma
    enter_outdoor_margin = enter_g / outdoor_stat_sigma
    print(f"  margin (enter / σ_stationary):")
    print(f"    indoor  : {enter_indoor_margin:.0f}× σ_stationary  "
          f"(plenty of headroom)")
    print(f"    outdoor : {enter_outdoor_margin:.0f}× σ_stationary  "
          f"({'still ok' if enter_outdoor_margin > 5 else 'TIGHT — may chatter'})")

    # ---- Plot -----------------------------------------------------
    fig, axes = plt.subplots(2, 2, figsize=(14, 8))

    # (1) Stationary time series, both surfaces, same y-scale
    fs_in = indoor_stat.derived_rate_hz
    t_in = np.arange(len(indoor_stat_hp)) / fs_in
    fs_out = outdoor_data["stationary"]["fs"]
    t_out = np.arange(len(outdoor_stat_hp)) / fs_out
    ax = axes[0, 0]
    ax.plot(t_in, indoor_stat_hp, color="steelblue", lw=0.5,
            label=f"indoor σ={indoor_stat_sigma*1000:.2f} mg")
    ax.plot(t_out, outdoor_stat_hp, color="darkorange", lw=0.5, alpha=0.8,
            label=f"outdoor σ={outdoor_stat_sigma*1000:.2f} mg")
    ax.set_xlabel("time [s]")
    ax.set_ylabel("|a|_HP [g]")
    ax.set_title("Stationary baseline — outdoor vs indoor "
                 f"({outdoor_stat_sigma/indoor_stat_sigma:.0f}× higher σ)")
    ax.set_ylim(-0.05, 0.05)
    ax.grid(alpha=0.3)
    ax.legend(fontsize=8)

    # (2) Stationary PSDs
    nperseg_in = min(400, len(indoor_stat_hp))
    f_in, p_in = signal.welch(indoor_stat_hp, fs=fs_in, nperseg=nperseg_in,
                               window="hann")
    nperseg_out = min(400, len(outdoor_stat_hp))
    f_out, p_out = signal.welch(outdoor_stat_hp, fs=fs_out, nperseg=nperseg_out,
                                 window="hann")
    ax = axes[0, 1]
    ax.semilogy(f_in, p_in, color="steelblue", lw=1.0, label="indoor stationary")
    ax.semilogy(f_out, p_out, color="darkorange", lw=1.0, label="outdoor stationary")
    ax.set_xlabel("frequency [Hz]")
    ax.set_ylabel("PSD of |a|_HP [g²/Hz]")
    ax.set_title("Stationary PSD — where does the outdoor noise live?")
    ax.set_xlim(0, 50)
    ax.grid(alpha=0.3, which="both")
    ax.legend(fontsize=9)

    # (3) Walk |a|_HP σ distributions
    ax = axes[1, 0]
    ax.boxplot([indoor_walk_sigmas, [s for _, _, s in outdoor_walk_sigmas]],
               labels=["indoor walks", "outdoor walks"], widths=0.5)
    ax.scatter(np.ones(len(indoor_walk_sigmas)),
               indoor_walk_sigmas, color="steelblue", alpha=0.7, s=40, zorder=3)
    out_sig = [s for _, _, s in outdoor_walk_sigmas]
    ax.scatter(np.full(len(out_sig), 2),
               out_sig, color="darkorange", alpha=0.7, s=40, zorder=3)
    ax.set_ylabel("σ |a|_HP [g] (middle half of session)")
    ax.set_title("Walking signal magnitude — indoor vs outdoor")
    ax.grid(alpha=0.3, axis="y")

    # (4) Walking PSD: one normal-speed run per surface, same protocol
    indoor_normal = next(r for r in indoor_walks if r.run_idx == 3)  # 20ft normal
    indoor_normal_hp = indoor_mag_hp(indoor_normal)
    f_in_w, p_in_w = signal.welch(indoor_normal_hp, fs=indoor_normal.derived_rate_hz,
                                    nperseg=400, window="hann")
    outdoor_normal = outdoor_data["13 normal"]
    f_out_w, p_out_w = signal.welch(outdoor_normal["mag_hp"], fs=outdoor_normal["fs"],
                                      nperseg=400, window="hann")
    ax = axes[1, 1]
    ax.semilogy(f_in_w, p_in_w, color="steelblue", lw=1.0,
                 label="indoor 20ft normal (run 3)")
    ax.semilogy(f_out_w, p_out_w, color="darkorange", lw=1.0,
                 label="outdoor 20ft normal (run 13)")
    ax.axvline(5, color="gray", ls="--", lw=0.7,
                label="LP cutoff 5 Hz")
    ax.set_xlabel("frequency [Hz]")
    ax.set_ylabel("PSD of |a|_HP [g²/Hz]")
    ax.set_title("Walking PSD — does outdoor add HF content?")
    ax.set_xlim(0, 50)
    ax.grid(alpha=0.3, which="both")
    ax.legend(fontsize=8)

    fig.suptitle("Outdoor concrete sidewalk vs indoor polished concrete — "
                 "surface vibration impact", fontsize=11)
    fig.tight_layout()
    out = FIG_DIR / "outdoor_vs_indoor.png"
    fig.savefig(out, dpi=140, bbox_inches="tight")
    plt.close(fig)
    print(f"\nFigure: {out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

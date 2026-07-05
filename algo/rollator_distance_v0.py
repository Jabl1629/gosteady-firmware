#!/usr/bin/env python3
"""Rollator distance estimator v0 — surface-normalized vibration odometry.

distance = Σ_active (window vibration RMS) · dt / (c · roughness), where roughness
is the per-run spectral flatness of the wheel-vibration (a speed-independent surface
descriptor; see rollator_roughness_probe). Compares three models by leave-one-out:

  naive     distance = m · vib_int                     (single constant, no surface)
  oracle    distance = m_surface · vib_int             (true surface label picks m)
  flatness  distance = m · (vib_int / flatness)        (continuous roughness, NO label)

Data: raw_sessions/2026-07-04-dt1-rollator (16 recovered runs; 8 clean 'normal'
walks over 2 surfaces). GT = taped course distance. HEAVILY sample-limited — this
is a mechanism demonstration + harness, not a validated model. Re-run when carpet
+ more speed/distance reps land.  Run:  algo/venv/bin/python algo/rollator_distance_v0.py
"""
import sys, numpy as np
from scipy import signal
sys.path.insert(0, ".")
from algo.data_loader import load_capture_day
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt

G = 9.80665; FS = 100.0; W = int(0.5 * FS)
MOTION_ENTER = 0.01           # g, std of HP-accel per 0.5 s window
DIR = "raw_sessions/2026-07-04-dt1-rollator"


def hp_accel_g(imu):
    a = np.sqrt(imu["ax"].astype(float)**2 + imu["ay"].astype(float)**2
                + imu["az"].astype(float)**2) / G
    b, ba = signal.butter(2, 0.2 / (FS / 2), btype="high")
    return signal.filtfilt(b, ba, a)


def extract(run):
    """Per-run features for the odometer. None if not a clean walking run."""
    imu = run.imu; n = len(imu); hp = hp_accel_g(imu)
    rms_w = np.array([np.std(hp[i:i+W]) for i in range(0, n - W, W)])
    active = rms_w > MOTION_ENTER
    if np.sqrt(np.mean(hp**2)) < 0.006 or active.sum() < 8:
        return None
    if run.annotations["run_type"] != "normal":     # cleanest distance runs
        return None
    walk_t = float(active.sum() * 0.5)
    vib_int = float(rms_w[active].sum() * 0.5)        # Σ speed-proxy · dt
    # roughness = spectral flatness over active samples (speed-independent)
    m = np.zeros(n, bool)
    for wi, a_ in enumerate(active):
        if a_: m[wi*W:(wi+1)*W] = True
    f, p = signal.welch(hp[m], FS, nperseg=min(512, int(m.sum()))); p = p + 1e-15
    flatness = float(np.exp(np.mean(np.log(p))) / np.mean(p))
    dist = float(run.annotations.get("intended_distance_ft") or 0)
    return dict(uuid=str(run.header.session_uuid)[:8], surf=run.annotations["surface"],
                dist=dist, walk_t=walk_t, vib_int=vib_int, flatness=flatness,
                avg_speed=round(dist/walk_t, 2) if walk_t else 0)


def fit_m(rows, feat):                # LS through origin: dist ≈ m · feat
    x = np.array([feat(r) for r in rows]); y = np.array([r["dist"] for r in rows])
    return float(np.sum(x*y) / np.sum(x*x))


def loo(rows, feat, per_surface=False):
    """Leave-one-out predictions + MAPE."""
    preds = []
    for i, test in enumerate(rows):
        train = [r for j, r in enumerate(rows) if j != i]
        if per_surface:
            train = [r for r in train if r["surf"] == test["surf"]]
        m = fit_m(train, feat)
        preds.append(m * feat(test))
    preds = np.array(preds); actual = np.array([r["dist"] for r in rows])
    mape = float(np.mean(np.abs(preds - actual) / actual) * 100)
    return preds, mape


def main():
    rows = [f for f in (extract(r) for r in load_capture_day(DIR)) if f]
    rows.sort(key=lambda r: (r["surf"], r["dist"], r["avg_speed"]))
    print(f"clean 'normal' walking runs: {len(rows)} "
          f"({sum(r['surf']=='polished_concrete' for r in rows)} polished, "
          f"{sum(r['surf']=='outdoor_concrete' for r in rows)} outdoor)\n")
    print(f"{'uuid':>9} {'surface':>17} {'GTdist':>6} {'walk_t':>6} {'avg_sp':>6} "
          f"{'vib_int':>7} {'flatns':>6} {'vib/flat':>8}")
    for r in rows:
        print(f"{r['uuid']:>9} {r['surf']:>17} {r['dist']:>6.0f} {r['walk_t']:>6.1f} "
              f"{r['avg_speed']:>6} {r['vib_int']:>7.3f} {r['flatness']:>6.3f} "
              f"{r['vib_int']/r['flatness']:>8.3f}")

    models = {
        "naive (no surface) ": (lambda r: r["vib_int"], False),
        "oracle (true label)": (lambda r: r["vib_int"], True),
        "flatness-normalized": (lambda r: r["vib_int"]/r["flatness"], False),
    }
    print("\n=== leave-one-out MAPE vs taped ground truth ===")
    results = {}
    for name, (feat, ps) in models.items():
        preds, mape = loo(rows, feat, ps)
        results[name] = (preds, mape)
        # per-surface breakdown
        po = np.array([r["surf"] == "polished_concrete" for r in rows])
        act = np.array([r["dist"] for r in rows])
        mp = np.mean(np.abs(preds[po]-act[po])/act[po])*100
        mo = np.mean(np.abs(preds[~po]-act[~po])/act[~po])*100
        print(f"  {name}:  overall {mape:5.1f}%   (polished {mp:4.1f}%  outdoor {mo:5.1f}%)")

    # figure: predicted vs actual, naive vs flatness
    fig, axs = plt.subplots(1, 2, figsize=(9.2, 4.4), sharex=True, sharey=True)
    act = np.array([r["dist"] for r in rows])
    cols = ["#1f77b4" if r["surf"] == "polished_concrete" else "#ff7f0e" for r in rows]
    for ax, name in zip(axs, ["naive (no surface) ", "flatness-normalized"]):
        preds = results[name][0]
        ax.scatter(act + np.random.uniform(-.3, .3, len(act)), preds, c=cols, s=70,
                   edgecolor="k", lw=.5, zorder=3)
        ax.plot([0, 30], [0, 30], "--", c="gray", lw=1)
        ax.set_title(f"{name.strip()}\nLOO MAPE {results[name][1]:.0f}%", fontsize=10)
        ax.set_xlabel("taped distance (ft)"); ax.set_xlim(0, 30); ax.set_ylim(0, 45)
        ax.grid(alpha=.3)
    axs[0].set_ylabel("predicted distance (ft)")
    axs[0].scatter([], [], c="#1f77b4", label="polished"); axs[0].scatter([], [], c="#ff7f0e", label="outdoor")
    axs[0].legend(fontsize=8, loc="upper left")
    fig.suptitle("Surface normalization rescues the outdoor runs (orange)", fontsize=11)
    fig.tight_layout(); fig.savefig("algo/figures/rollator/F_distance_eval.png", dpi=130)
    print("\nwrote algo/figures/rollator/F_distance_eval.png")


if __name__ == "__main__":
    main()

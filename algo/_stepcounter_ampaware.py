"""Amplitude-aware merge: absorb a close peak only if it's a SMALL satellite
of the previous step; keep it if it's full-sized (a genuine fast step).

Rule (forward greedy over the 0.5s peak train; distance still uses all peaks):
  gap = t - t_last_kept
  keep as new step if   gap >= merge_gap            (clearly separated)
                  or    amp >= ratio * ref_amp      (full-sized, even if close)
  else drop (small + close => satellite sub-impulse)
ref_amp = amplitude of the last kept step (firmware-cheap, O(peaks)).

Run: algo/venv/bin/python3 -m algo._stepcounter_ampaware
"""
from __future__ import annotations

import warnings
import numpy as np

from .data_loader import load_capture_day
from ._stepcounter_proto import run_pipeline, count_refractory_merge

warnings.simplefilter("ignore")

DAYS = ["raw_sessions/2026-04-23", "raw_sessions/2026-04-25"]
GAP_GRID = [0.8, 1.0, 1.2]
RATIO_GRID = [0.5, 0.6, 0.7]


def count_ampaware(times, amps, merge_gap, ratio):
    if len(times) == 0:
        return 0
    kept = 1
    t_last, a_ref = times[0], amps[0]
    for t, a in zip(times[1:], amps[1:]):
        gap = t - t_last
        if gap >= merge_gap or a >= ratio * a_ref:
            kept += 1
            t_last, a_ref = t, a
        # else: drop satellite (t_last/a_ref unchanged)
    return kept


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
            times, amps, _ = run_pipeline(r.accel_mag_g, fs)
            rows.append({"speed": r.intended_speed, "manual": int(man),
                         "times": times, "amps": amps})

    man = np.array([x["manual"] for x in rows], float)

    def stats(predfn):
        pred = np.array([predfn(x) for x in rows], float)
        mape = np.mean(np.abs(pred - man) / man) * 100
        rr = pred / man
        per = {}
        for sp in ["slow", "normal", "fast"]:
            idx = [i for i, x in enumerate(rows) if x["speed"] == sp]
            if idx:
                per[sp] = np.mean([abs(pred[i] - man[i]) / man[i] * 100 for i in idx])
        return mape, rr.mean(), rr.std(), per

    # Baselines
    print(f"{'method':<26} {'MAPE':>6} {'rmean':>6} {'rstd':>5}  "
          f"{'slow':>6} {'normal':>7} {'fast':>6}")

    def line(name, predfn):
        m, rm, rs, per = stats(predfn)
        print(f"{name:<26} {m:>5.1f}% {rm:>6.2f} {rs:>5.2f}  "
              f"{per.get('slow', float('nan')):>5.1f}% {per.get('normal', float('nan')):>6.1f}% "
              f"{per.get('fast', float('nan')):>5.1f}%")

    line("current (0.5s)", lambda x: len(x["times"]))
    line("plain merge 0.8s", lambda x: count_refractory_merge(x["times"], 0.8))
    print("-" * 70)
    best = None
    for g in GAP_GRID:
        for r in RATIO_GRID:
            name = f"ampaware g={g} ratio={r}"
            line(name, lambda x, g=g, r=r: count_ampaware(x["times"], x["amps"], g, r))
            m, *_ = stats(lambda x, g=g, r=r: count_ampaware(x["times"], x["amps"], g, r))
            if best is None or m < best[0]:
                best = (m, g, r)
    print("-" * 70)
    print(f"best amp-aware: MAPE={best[0]:.1f}% at merge_gap={best[1]}s ratio={best[2]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

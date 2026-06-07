"""Prototype decoupled step counters and measure vs manual counts.

The distance pipeline is left UNTOUCHED (still consumes the full 0.5s peak
train). These counters only change the reported step number:

  A. refractory-merge: from the 0.5s peak train, keep a peak only if it is
     >= merge_gap from the last KEPT peak. (Provably == bumping detector
     min_gap, but applied to the count only — distance keeps all peaks.)
  B. autocorr-cadence: estimate the dominant gait period from the
     autocorrelation of the LP signal over the walking span, then
     steps ~= span / period.

Run: algo/venv/bin/python3 -m algo._stepcounter_proto
"""
from __future__ import annotations

import warnings
import numpy as np

from .data_loader import load_capture_day
from .filters import butterworth_hp, butterworth_lp
from .step_detector import StepDetector

warnings.simplefilter("ignore")

DAYS = ["raw_sessions/2026-04-23", "raw_sessions/2026-04-25"]
HP_HZ, LP_HZ, ORDER = 0.2, 5.0, 2
ENTER, EXIT, MAXACT, BASE_GAP = 0.02, 0.005, 5.0, 0.5
MERGE_SWEEP = [0.7, 0.8, 0.9, 1.0]
# Plausible elderly-walker step period band (33-100 steps/min).
AC_PERIOD_MIN_S, AC_PERIOD_MAX_S = 0.6, 1.8


def run_pipeline(mag_g: np.ndarray, fs: float):
    """Return (peak_times_s, peak_amps, mag_lp) using the SHIPPED 0.5s detector."""
    hp = butterworth_hp(HP_HZ, fs=fs, order=ORDER)
    lp = butterworth_lp(LP_HZ, fs=fs, order=ORDER)
    hp.init_steady(float(mag_g[0]) - 1.0)
    det = StepDetector(fs=fs, enter_threshold_g=ENTER, exit_threshold_g=EXIT,
                       min_gap_s=BASE_GAP, max_active_s=MAXACT)
    mag_lp = np.empty(len(mag_g))
    times, amps = [], []
    for i in range(len(mag_g)):
        v_hp = hp.step(float(mag_g[i]) - 1.0)
        v_lp = lp.step(v_hp)
        mag_lp[i] = v_lp
        p = det.step(v_lp)
        if p is not None:
            times.append(p.time_s)
            amps.append(p.amplitude_g)
    return np.array(times), np.array(amps), mag_lp


def count_refractory_merge(times: np.ndarray, merge_gap_s: float) -> int:
    if len(times) == 0:
        return 0
    kept = 1
    last = times[0]
    for t in times[1:]:
        if t - last >= merge_gap_s:
            kept += 1
            last = t
    return kept


def count_autocorr(times: np.ndarray, mag_lp: np.ndarray, fs: float) -> int:
    if len(times) < 2:
        return len(times)
    i0 = int(round(times[0] * fs))
    i1 = int(round(times[-1] * fs))
    seg = mag_lp[max(i0, 0):min(i1 + 1, len(mag_lp))].astype(np.float64)
    if len(seg) < int(AC_PERIOD_MAX_S * fs) + 5:
        # too short for a reliable period — fall back to refractory merge
        return count_refractory_merge(times, 0.9)
    seg = seg - seg.mean()
    ac = np.correlate(seg, seg, mode="full")
    ac = ac[len(ac) // 2:]
    if ac[0] <= 0:
        return count_refractory_merge(times, 0.9)
    ac = ac / ac[0]
    lo = int(AC_PERIOD_MIN_S * fs)
    hi = min(int(AC_PERIOD_MAX_S * fs), len(ac) - 1)
    if hi <= lo:
        return count_refractory_merge(times, 0.9)
    period_samples = lo + int(np.argmax(ac[lo:hi + 1]))
    period_s = period_samples / fs
    span_s = times[-1] - times[0]
    return int(round(span_s / period_s)) + 1


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
            times, amps, mag_lp = run_pipeline(r.accel_mag_g, fs)
            rows.append({
                "speed": r.intended_speed, "type": r.run_type,
                "manual": int(man), "cur": len(times),
                "merge": {g: count_refractory_merge(times, g) for g in MERGE_SWEEP},
                "ac": count_autocorr(times, mag_lp, fs),
            })

    man = np.array([x["manual"] for x in rows], float)

    def mape(pred):
        pred = np.asarray(pred, float)
        return np.mean(np.abs(pred - man) / man) * 100

    def ratio(pred):
        pred = np.asarray(pred, float)
        return pred / man

    print(f"\n{'speed':<7} {'type':<10} {'man':>4} {'cur':>4} "
          + " ".join(f"m{g}".rjust(4) for g in MERGE_SWEEP) + f" {'ac':>4}")
    for x in rows:
        print(f"{x['speed']:<7} {x['type']:<10} {x['manual']:>4} {x['cur']:>4} "
              + " ".join(str(x['merge'][g]).rjust(4) for g in MERGE_SWEEP)
              + f" {x['ac']:>4}")

    print(f"\n{'method':<22} {'MAPE':>6} {'ratio mean':>11} {'ratio std':>10}")
    cur = [x["cur"] for x in rows]
    print(f"{'current (0.5s)':<22} {mape(cur):>5.1f}% {ratio(cur).mean():>11.2f} "
          f"{ratio(cur).std():>10.2f}")
    for g in MERGE_SWEEP:
        pred = [x["merge"][g] for x in rows]
        print(f"{'merge ' + str(g) + 's':<22} {mape(pred):>5.1f}% "
              f"{ratio(pred).mean():>11.2f} {ratio(pred).std():>10.2f}")
    ac = [x["ac"] for x in rows]
    print(f"{'autocorr-cadence':<22} {mape(ac):>5.1f}% {ratio(ac).mean():>11.2f} "
          f"{ratio(ac).std():>10.2f}")

    # Per-speed breakdown: current vs best merge vs autocorr
    print(f"\nper-speed mean |error|% (worst case = slow):")
    best_g = min(MERGE_SWEEP, key=lambda g: mape([x['merge'][g] for x in rows]))
    print(f"  (best merge gap = {best_g}s)")
    print(f"  {'speed':<7} {'n':>2} {'current':>8} {'merge'+str(best_g):>9} {'autocorr':>9}")
    for sp in ["slow", "normal", "fast"]:
        idx = [i for i, x in enumerate(rows) if x["speed"] == sp]
        if not idx:
            continue
        def ape(key, g=None):
            vals = []
            for i in idx:
                p = rows[i]["merge"][g] if g else rows[i][key]
                vals.append(abs(p - rows[i]["manual"]) / rows[i]["manual"] * 100)
            return np.mean(vals)
        print(f"  {sp:<7} {len(idx):>2} {ape('cur'):>7.1f}% "
              f"{ape(None, best_g):>8.1f}% {ape('ac'):>8.1f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

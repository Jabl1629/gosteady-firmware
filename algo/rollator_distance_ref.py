#!/usr/bin/env python3
"""Rollator distance estimator — streaming GOLDEN REFERENCE (the spec the C port
mirrors, and the source of its coefficients + host-test vectors).

Recipe (validated 2026-07-05, causal, ~30.6% LOO MAPE, 3 surfaces, 0.55-3.08 ft/s):
  - CAUSAL biquads (= C single-pass), no FFT.
  - wheel-band 10-44Hz (gate) + HP 0.2Hz (amplitude) + 16 log-band bank 1-49Hz.
  - per 500ms window: wheel RMS -> Schmitt gate (enter .010 / exit .006 / 4-window
    debounce); HP RMS -> vib_int over active windows.
  - per 2s window (>=60% active): flatness_w = geomean(bandE)/mean(bandE);
    session flatness = geomean over active 2s windows.
  - distance = m * vib_int / flatness.

The firmware buffers per-window features and resolves the gate + flat-norm at
finalize (mirrored here). Run `python algo/rollator_distance_ref.py --emit-c` to
print the C coefficient block; default validates MAPE across all capture days.
"""
import sys, glob, os, math, argparse
import numpy as np
from scipy import signal
sys.path.insert(0, ".")
from algo.data_loader import load_capture_day

G = 9.80665; FS = 100.0
W = 50                     # 500 ms gate window (samples)
FLAT_W = 4                 # 2 s flatness window (gate windows)
N_BANDS = 16
BAND_LO, BAND_HI = 1.0, 49.0
WHEEL_LO, WHEEL_HI = 10.0, 44.0
HP_FC = 0.2
GATE_ENTER, GATE_EXIT = 0.010, 0.006
GATE_DEBOUNCE = 4          # windows below-exit to confirm a pause
FLAT_ACTIVE_FRAC = 0.60
MIN_ACTIVE_WINDOWS = 4
DIST_M = 1.1598            # fit on 2026-07-04+05 (n=39, causal, 31.7% LOO); --fit to refresh

BAND_EDGES = np.geomspace(BAND_LO, BAND_HI, N_BANDS + 1)

def _sos_bp(lo, hi, order): return signal.butter(order, [lo/(FS/2), hi/(FS/2)], btype="band", output="sos")
def _sos_hp(fc, order):     return signal.butter(order, fc/(FS/2), btype="high", output="sos")

WHEEL_SOS = _sos_bp(WHEEL_LO, WHEEL_HI, 2)          # order-4 -> 2 sections
HP_SOS    = _sos_hp(HP_FC, 2)                        # order-2 -> 1 section
BAND_SOS  = [_sos_bp(BAND_EDGES[k], BAND_EDGES[k+1], 2) for k in range(N_BANDS)]  # 2 sections each


def gate(wheel_rms):
    """Schmitt + debounce, identical to rollator_surface_classifier.gate."""
    active = np.zeros(len(wheel_rms), bool); state = False; still = 0
    for i, v in enumerate(wheel_rms):
        if state:
            if v < GATE_EXIT:
                still += 1
                if still >= GATE_DEBOUNCE:
                    state = False; active[i-still+1:i+1] = False
                else:
                    active[i] = True
            else:
                still = 0; active[i] = True
        else:
            if v > GATE_ENTER:
                state = True; still = 0; active[i] = True
    return active


def compute(mag_g):
    """Streaming-equivalent (causal) distance for one session's mag_g stream.
    Returns dict with distance components + validity."""
    wheel = signal.sosfilt(WHEEL_SOS, mag_g)
    hp    = signal.sosfilt(HP_SOS, mag_g)
    bands = [signal.sosfilt(sos, mag_g) for sos in BAND_SOS]
    n = len(mag_g); nwin = n // W
    wheel_rms = np.array([np.std(wheel[w*W:(w+1)*W]) for w in range(nwin)])
    hp_rms    = np.array([np.std(hp[w*W:(w+1)*W])    for w in range(nwin)])
    active = gate(wheel_rms)
    walk_t = float(active.sum() * 0.5)
    vib_int = float(np.sum(hp_rms[active]) * 0.5)
    logs = []
    for f0 in range(0, nwin - FLAT_W + 1, FLAT_W):
        if active[f0:f0+FLAT_W].mean() < FLAT_ACTIVE_FRAC:
            continue
        s, e = f0*W, (f0+FLAT_W)*W
        E = np.array([np.mean(b[s:e]**2) for b in bands]) + 1e-15
        logs.append(math.log(math.exp(np.mean(np.log(E))) / np.mean(E)))
    valid = (active.sum() >= MIN_ACTIVE_WINDOWS) and len(logs) > 0 and vib_int > 0
    flatness = math.exp(np.mean(logs)) if logs else float("nan")
    distance = (DIST_M * vib_int / flatness) if valid else float("nan")
    return dict(valid=valid, distance_ft=distance, flatness=flatness,
                walk_t_s=walk_t, vib_int=vib_int, n_active=int(active.sum()))


def _mag_of(run):
    imu = run.imu
    return np.sqrt(imu["ax"].astype(float)**2 + imu["ay"].astype(float)**2
                   + imu["az"].astype(float)**2) / G


def load_walking():
    out = []
    for d in sorted(glob.glob("raw_sessions/2026-07-0*-dt1-rollator")):
        if not glob.glob(os.path.join(d, "capture_*.csv")): continue
        for r in load_capture_day(d):
            ann = r.annotations
            if str(ann.get("valid","?")).upper() == "N" or "pause" in str(ann.get("course_id","")):
                continue
            if len(r.imu) < 5*FS: continue
            out.append((r, float(ann.get("intended_distance_ft") or 0), ann.get("surface","?")))
    return out


def validate(fit=False):
    runs = load_walking()
    feats = []
    for r, dist, surf in runs:
        c = compute(_mag_of(r))
        if c["valid"]: feats.append((c["vib_int"]/c["flatness"], dist, surf, str(r.header.session_uuid)[:8]))
    x = np.array([f[0] for f in feats]); y = np.array([f[1] for f in feats])
    m = float(np.sum(x*y)/np.sum(x*x))
    # LOO MAPE at the current global m-fit
    errs = []
    for i in range(len(feats)):
        tr = [feats[j] for j in range(len(feats)) if j != i]
        xt = np.array([t[0] for t in tr]); yt = np.array([t[1] for t in tr])
        mi = np.sum(xt*yt)/np.sum(xt*xt)
        errs.append(abs(mi*feats[i][0]-feats[i][1])/feats[i][1])
    print(f"reference: n={len(feats)} valid walking runs; global m={m:.4f}; "
          f"LOO MAPE={100*np.mean(errs):.1f}%")
    if fit:
        print(f"  -> set DIST_M = {m:.4f}")
    return m


def emit_c():
    def sos_c(name, sos):
        print(f"/* {name}: {len(sos)} biquad section(s) [b0,b1,b2,a1,a2] */")
        print(f"static const float {name}[{len(sos)}][5] = {{")
        for s in sos:
            print(f"\t{{ {s[0]:+.8e}f, {s[1]:+.8e}f, {s[2]:+.8e}f, {-s[4]:+.8e}f, {-s[5]:+.8e}f }},")
        print("};")
    print(f"/* AUTO-GENERATED by algo/rollator_distance_ref.py. Recipe: 16 log-bands "
          f"{BAND_LO}-{BAND_HI}Hz, wheel {WHEEL_LO}-{WHEEL_HI}Hz gate, HP {HP_FC}Hz. */")
    print(f"#define GS_ROLL_DIST_M            {DIST_M:.6f}f")
    print(f"#define GS_ROLL_GATE_ENTER_G      {GATE_ENTER:.6f}f")
    print(f"#define GS_ROLL_GATE_EXIT_G       {GATE_EXIT:.6f}f")
    print(f"#define GS_ROLL_GATE_DEBOUNCE     {GATE_DEBOUNCE}u")
    print(f"#define GS_ROLL_FLAT_ACTIVE_FRAC  {FLAT_ACTIVE_FRAC:.2f}f")
    print(f"#define GS_ROLL_MIN_ACTIVE_WIN    {MIN_ACTIVE_WINDOWS}u")
    sos_c("gs_roll_wheel_sos", WHEEL_SOS)
    sos_c("gs_roll_hp_sos", HP_SOS)
    print(f"/* 16 bands x 2 sections, flattened [band][section][5] */")
    print(f"static const float gs_roll_band_sos[{N_BANDS}][2][5] = {{")
    for k in range(N_BANDS):
        secs = BAND_SOS[k]
        print(f"\t{{ /* band {k}: {BAND_EDGES[k]:.2f}-{BAND_EDGES[k+1]:.2f} Hz */")
        for s in secs:
            print(f"\t\t{{ {s[0]:+.8e}f, {s[1]:+.8e}f, {s[2]:+.8e}f, {-s[4]:+.8e}f, {-s[5]:+.8e}f }},")
        print("\t},")
    print("};")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit-c", action="store_true"); ap.add_argument("--fit", action="store_true")
    a = ap.parse_args()
    if a.emit_c: emit_c()
    else: validate(fit=a.fit)

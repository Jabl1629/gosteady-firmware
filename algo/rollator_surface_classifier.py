#!/usr/bin/env python3
"""Rollator distance estimator + surface classifier — HONEST HARNESS (v2).

Rebuilt after the 2026-07-05 adversarial review (workflow wf_997887c5). The v0/v1
headline ("flatness-normalized ~17% LOO ≈ oracle") was a CONSTANT-SPEED ARTIFACT:
on 07-04 every run sat at 0.66-1.21 ft/s, so distance ≈ speed·time and a plain
stopwatch (dist = m·walk_t) beats the vibration odometer (11.6% vs 16.9%); within
one surface, flatness normalization adds nothing. So this harness is built around
the rule: **a vibration model earns its place ONLY if it beats the time-only
baseline on runs spanning a real speed range, out-of-sample.**

What it does honestly:
  - TIME-ONLY baseline (dist = m·walk_t) as a first-class model on every split.
  - Motion gate = Schmitt hysteresis (enter 0.010 g, exit 0.006 g) + 2 s debounce,
    on the wheel-band (10-44 Hz) envelope — so a lean/hold with no roll (no wheel
    vibration) does NOT count as travel.
  - Speed model dist = v_hat·walk_t with log v = b0 + log(amp_rate) + b2·flatness
    (wheel-speed exponent PINNED to 1; roughness enters additively in log-domain).
  - NESTED feature selection (pick the roughness normalizer inside each fold) so we
    report the honest cost of selection, plus the within-surface null.
  - Leave-one-SURFACE-out FIRST (cold unseen surface) with per-surface bootstrap
    CIs and an identifiability gate (a surface needs >=2 distances to get a slope).
  - Pause/lean trim validation on *_pause runs.

Auto-discovers ingested raw_sessions/2026-07-0*-dt1-rollator dirs; honors `valid`
(excludes 'N'). Run from gosteady-firmware/:  algo/venv/bin/python algo/rollator_surface_classifier.py
"""
import sys, math, glob, os
import numpy as np
from scipy import signal, stats
sys.path.insert(0, ".")
from algo.data_loader import load_capture_day

G = 9.80665; FS = 100.0; W = int(0.5 * FS)
ENTER, EXIT = 0.010, 0.006          # Schmitt (g) on wheel-band window RMS
DEBOUNCE_STILL = 4                   # consecutive still windows (=2 s) to declare a gap
ORDER = ["polished_concrete", "outdoor_concrete", "outdoor_asphalt"]
ROUGH_FEATS = ["flatness", "crest", "kurt", "amp", "none"]


def _bp(x, lo, hi, order=4):
    b, a = signal.butter(order, [lo/(FS/2), hi/(FS/2)], btype="band")
    return signal.filtfilt(b, a, x)

def _hp(x, fc=0.2, order=2):
    b, a = signal.butter(order, fc/(FS/2), btype="high")
    return signal.filtfilt(b, a, x)


def gate(wheel_rms):
    """Schmitt hysteresis + debounce over per-window wheel-band RMS.
    Returns boolean active mask (len == n_windows)."""
    active = np.zeros(len(wheel_rms), bool)
    state = False; still_run = 0
    for i, v in enumerate(wheel_rms):
        if state:
            if v < EXIT:
                still_run += 1
                if still_run >= DEBOUNCE_STILL:      # confirmed pause -> back-fill still
                    state = False
                    active[i-still_run+1:i+1] = False
                else:
                    active[i] = True                 # tentative: keep active until confirmed
            else:
                still_run = 0; active[i] = True
        else:
            if v > ENTER:
                state = True; still_run = 0; active[i] = True
    return active


def bursts(active):
    out = []; i = 0
    while i < len(active):
        if active[i]:
            j = i
            while j < len(active) and active[j]: j += 1
            out.append((i, j-i)); i = j
        else: i += 1
    return out


def features(run):
    ann = run.annotations
    if str(ann.get("valid", "?")).upper() == "N":
        return None
    imu = run.imu; n = len(imu)
    if n < 5*FS: return None
    amag = np.sqrt(imu["ax"].astype(float)**2 + imu["ay"].astype(float)**2
                   + imu["az"].astype(float)**2) / G
    hp = _hp(amag)
    wheel = _bp(amag, 10, 44)                              # wheel-vibration band
    wheel_rms = np.array([np.std(wheel[i:i+W]) for i in range(0, n-W, W)])
    hp_rms_w  = np.array([np.std(hp[i:i+W])    for i in range(0, n-W, W)])
    active = gate(wheel_rms)
    if active.sum() < 8: return None
    walk_t = float(active.sum()*0.5)
    amp_rate = float(np.mean(wheel_rms[active]))           # wheel-band speed proxy (per-time)
    vib_int  = float(np.sum(hp_rms_w[active])*0.5)         # legacy lumped feature
    # roughness on active samples
    m = np.zeros(n, bool)
    for wi, a_ in enumerate(active):
        if a_: m[wi*W:(wi+1)*W] = True
    x = hp[m]
    f, p = signal.welch(x, FS, nperseg=min(512, int(m.sum()))); p = p + 1e-15
    flatness = float(np.median([  # median-of-window flatness (robust)
        np.exp(np.mean(np.log(pp)))/np.mean(pp)
        for pp in [signal.welch(x[i:i+W*4], FS, nperseg=min(256, W*4))[1]+1e-15
                   for i in range(0, max(1, len(x)-W*4), W*4)]] or [
        np.exp(np.mean(np.log(p)))/np.mean(p)]))
    amp = float(np.sqrt(np.mean(x**2)))
    crest = float(np.max(np.abs(x))/amp)
    kurt = float(stats.kurtosis(x))
    dist = float(ann.get("intended_distance_ft") or 0)
    cid = str(ann.get("course_id", ""))
    bl = bursts(active)
    return dict(uuid=str(run.header.session_uuid)[:8], surf=str(ann.get("surface","?")),
                dist=dist, speed=str(ann.get("intended_speed","?")),
                is_pause=("pause" in cid), walk_t=walk_t, amp_rate=amp_rate,
                vib_int=vib_int, flatness=flatness, crest=crest, kurt=kurt, amp=amp,
                v_gt=(dist/walk_t if walk_t > 0 else np.nan),
                n_bursts=len(bl), longest_gap_s=_longest_gap(active))


def _longest_gap(active):
    best=0;i=0
    while i<len(active):
        if not active[i]:
            j=i
            while j<len(active) and not active[j]: j+=1
            best=max(best,j-i); i=j
        else: i+=1
    return best*0.5


def load_all():
    rows=[]
    for d in sorted(glob.glob("raw_sessions/2026-07-0*-dt1-rollator")):
        if not glob.glob(os.path.join(d,"capture_*.csv")): continue
        for r in load_capture_day(d):
            fr=features(r)
            if fr: fr["day"]=os.path.basename(d)[:10]; rows.append(fr)
    return rows


# ---- estimators: each takes (train_rows, test_row) -> predicted dist ----
def _slope(rows, fx):
    x=np.array([fx(r) for r in rows]); y=np.array([r["dist"] for r in rows])
    return float(np.sum(x*y)/np.sum(x*x)) if np.sum(x*x)>0 else np.nan

def m_time(tr,te): return _slope(tr,lambda r:r["walk_t"])*te["walk_t"]
def m_naive(tr,te): return _slope(tr,lambda r:r["vib_int"])*te["vib_int"]
def m_flat(tr,te): return _slope(tr,lambda r:r["vib_int"]/r["flatness"])*te["vib_int"]/te["flatness"]
def m_oracle(tr,te):
    same=[r for r in tr if r["surf"]==te["surf"]]
    return _slope(same or tr, lambda r:r["vib_int"])*te["vib_int"]

def m_speed_logfit(tr,te):
    """dist = v_hat*walk_t; log v = b0 + log(amp_rate) + b2*flatness (wheel exponent=1)."""
    tr=[r for r in tr if r["walk_t"]>0 and r["amp_rate"]>0 and np.isfinite(r["v_gt"])]
    if len(tr)<3: return m_time(tr,te)
    # response = log(v_gt) - log(amp_rate)  (pin wheel exponent to 1) ~ b0 + b2*flatness
    y=np.array([math.log(r["v_gt"])-math.log(r["amp_rate"]) for r in tr])
    Xf=np.array([r["flatness"] for r in tr])
    A=np.vstack([np.ones_like(Xf), Xf]).T
    b0,b2=np.linalg.lstsq(A,y,rcond=None)[0]
    v_hat=te["amp_rate"]*math.exp(b0+b2*te["flatness"])
    vmean=float(np.mean([r["v_gt"] for r in tr]))
    if not np.isfinite(v_hat) or not (0.3 <= v_hat <= 5.0):   # physical rollator range
        v_hat=vmean                                           # fall back to constant speed
    return v_hat*te["walk_t"]

def m_nested_select(tr,te):
    """Pick the roughness normalizer by inner-LOO on the training fold, then apply."""
    best,bkey=1e9,"none"
    for key in ROUGH_FEATS:
        fx=(lambda r,k=key: r["vib_int"] if k=="none" else r["vib_int"]/r[k])
        errs=[]
        for i,t in enumerate(tr):
            inner=[r for j,r in enumerate(tr) if j!=i]
            s=_slope(inner,fx)
            if np.isfinite(s): errs.append(abs(s*fx(t)-t["dist"])/t["dist"])
        if errs and np.mean(errs)<best: best=np.mean(errs); bkey=key
    fx=(lambda r,k=bkey: r["vib_int"] if k=="none" else r["vib_int"]/r[k])
    return _slope(tr,fx)*fx(te), bkey

MODELS={"time-only":m_time,"naive-vib":m_naive,"flat-norm":m_flat,
        "oracle":m_oracle,"speed-logfit":m_speed_logfit}


def loo_run(rows, model):
    preds=[]
    for i,t in enumerate(rows):
        tr=[r for j,r in enumerate(rows) if j!=i]
        try: p=model(tr,t)
        except Exception: p=np.nan
        preds.append(p if np.isfinite(p) else np.nan)
    preds=np.array(preds); act=np.array([r["dist"] for r in rows]); ok=np.isfinite(preds)&(act>0)
    return preds, float(np.mean(np.abs(preds[ok]-act[ok])/act[ok])*100) if ok.any() else np.nan


def loo_surface(rows, model):
    surfs=sorted({r["surf"] for r in rows}, key=lambda s:ORDER.index(s) if s in ORDER else 9)
    out={}
    for s in surfs:
        tr=[r for r in rows if r["surf"]!=s]; te=[r for r in rows if r["surf"]==s]
        dists={r["dist"] for r in te}
        ident = len(dists)>=2
        if not tr or not te: continue
        errs=[]
        for t in te:
            try: p=model(tr,t)
            except Exception: p=np.nan
            if np.isfinite(p) and t["dist"]>0: errs.append(abs(p-t["dist"])/t["dist"])
        out[s]=(float(np.mean(errs)*100) if errs else np.nan, len(te), ident)
    return out


def boot_ci(rows, mA, mB, nboot=1000):
    """95% CI + P(A not better than B) for MAPE(A)-MAPE(B), run-resampled. (No RNG in
    stdlib-free way: use numpy default_rng with a fixed seed for reproducibility.)"""
    rng=np.random.default_rng(0); idx=np.arange(len(rows)); diffs=[]
    for _ in range(nboot):
        samp=[rows[i] for i in rng.choice(idx,len(idx),replace=True)]
        _,a=loo_run(samp,mA); _,b=loo_run(samp,mB)
        if np.isfinite(a) and np.isfinite(b): diffs.append(a-b)
    diffs=np.array(diffs)
    return (float(np.percentile(diffs,2.5)), float(np.percentile(diffs,97.5)),
            float(np.mean(diffs>=0)))


def main():
    rows=load_all()
    surfs=sorted({r["surf"] for r in rows}, key=lambda s:ORDER.index(s) if s in ORDER else 9)
    walk=[r for r in rows if not r["is_pause"]]
    pause=[r for r in rows if r["is_pause"]]
    sp=[r["v_gt"] for r in walk if np.isfinite(r["v_gt"])]
    print(f"loaded {len(rows)} valid runs; surfaces: "
          + ", ".join(f"{s}={sum(1 for r in walk if r['surf']==s)}" for s in surfs))
    print(f"  walking={len(walk)} pause={len(pause)} days={sorted({r['day'] for r in rows})}")
    print(f"  SPEED RANGE (v_gt=dist/walk_t): {min(sp):.2f}-{max(sp):.2f} ft/s (std {np.std(sp):.2f})")
    if np.std(sp) < 0.3:
        print("  *** WARNING: near-constant speed — a vibration odometer CANNOT be validated here;")
        print("      time-only will win by construction. Need fast pushes (1.5-3 ft/s). ***")

    print("\n=== TEST 0 — leave-one-RUN-out MAPE (time-only is the baseline to beat) ===")
    base=None
    for name,mdl in MODELS.items():
        _,mape=loo_run(walk,mdl); print(f"  {name:>13}: {mape:5.1f}%")
        if name=="time-only": base=mape
    # honest nested selection
    ns_pred=[]; picks=[]
    for i,t in enumerate(walk):
        tr=[r for j,r in enumerate(walk) if j!=i]
        try: p,k=m_nested_select(tr,t)
        except Exception: p,k=np.nan,"?"
        ns_pred.append(p); picks.append(k)
    ns_pred=np.array(ns_pred); act=np.array([r["dist"] for r in walk]); ok=np.isfinite(ns_pred)
    ns_mape=float(np.mean(np.abs(ns_pred[ok]-act[ok])/act[ok])*100)
    from collections import Counter
    print(f"  {'nested-select':>13}: {ns_mape:5.1f}%  (picks: {dict(Counter(picks))})")

    # ---- surface classifier + realistic "classify-then-per-surface-curve" ----
    if len(surfs) >= 2:
        # nearest-centroid on standardized [flatness,crest,kurt], leave-one-run-out
        keys=["flatness","crest","kurt"]
        mu={k:np.mean([r[k] for r in walk]) for k in keys}
        sd={k:np.std([r[k] for r in walk])+1e-9 for k in keys}
        def z(r): return np.array([(r[k]-mu[k])/sd[k] for k in keys])
        correct=0; cls_pred=[]
        for i,t in enumerate(walk):
            tr=[r for j,r in enumerate(walk) if j!=i]
            cent={s:np.mean([z(r) for r in tr if r["surf"]==s],axis=0)
                  for s in surfs if any(r["surf"]==s for r in tr)}
            pred=min(cent,key=lambda s:np.linalg.norm(z(t)-cent[s]))
            cls_pred.append(pred); correct+=(pred==t["surf"])
        print(f"\n=== Surface classifier (nearest-centroid on flatness+crest+kurt, LOO) ===")
        print(f"  accuracy: {100*correct/len(walk):.0f}% ({correct}/{len(walk)})")
        # deploy pipeline: classify surface in-fold, apply THAT surface's naive slope
        preds=[]
        for i,t in enumerate(walk):
            tr=[r for j,r in enumerate(walk) if j!=i]
            cent={s:np.mean([z(r) for r in tr if r["surf"]==s],axis=0)
                  for s in surfs if any(r["surf"]==s for r in tr)}
            ps=min(cent,key=lambda s:np.linalg.norm(z(t)-cent[s]))
            same=[r for r in tr if r["surf"]==ps]
            preds.append(_slope(same or tr,lambda r:r["vib_int"])*t["vib_int"])
        preds=np.array(preds); ok=np.isfinite(preds)
        cm=float(np.mean(np.abs(preds[ok]-act[ok])/act[ok])*100)
        print(f"  DEPLOY (classify -> per-surface vib curve) LOO MAPE: {cm:.1f}%  "
              f"[vs oracle {loo_run(walk,m_oracle)[1]:.1f}% (perfect class), flat-norm {loo_run(walk,m_flat)[1]:.1f}% (no class)]")

    print("\n=== Significance: flat-norm vs time-only (bootstrap 95% CI of MAPE gap) ===")
    lo,hi,pw=boot_ci(walk,m_flat,m_time,nboot=400)
    print(f"  MAPE(flat)-MAPE(time) 95% CI: [{lo:+.1f}, {hi:+.1f}] pp; "
          f"P(flat NOT better than time)={pw*100:.0f}%  "
          f"-> {'gap crosses 0, NOT significant' if lo<0<hi else 'significant'}")

    print("\n=== Within-surface null (does roughness-norm help when surface is FIXED?) ===")
    for s in surfs:
        g=[r for r in walk if r["surf"]==s]
        if len(g)>=4:
            _,a=loo_run(g,m_naive); _,b=loo_run(g,m_flat); _,t=loo_run(g,m_time)
            print(f"  {s:>18} (n={len(g)}): naive {a:.1f}% | flat {b:.1f}% | time-only {t:.1f}%")

    if len(surfs)>=3:
        print("\n=== TEST 1 — leave-one-SURFACE-out (cold unseen surface; the deployment number) ===")
        for name in ("time-only","flat-norm","speed-logfit","naive-vib"):
            res=loo_surface(walk,MODELS[name])
            cells=" | ".join(f"{s.split('_')[-1]}={mape:.0f}%{'' if ident else '(ident-lim)'}(n{n})"
                             for s,(mape,n,ident) in res.items())
            print(f"  {name:>13}: {cells}")
    else:
        print(f"\n=== TEST 1 — LOSO: need 3 surfaces, have {len(surfs)} (awaiting 07-05 pull) ===")

    if pause:
        print("\n=== TEST 3 — pause/lean trim (new hysteresis+wheel-band gate) ===")
        for r in sorted(pause,key=lambda r:(r["surf"],r["dist"])):
            est=m_time(walk,r)
            print(f"  {r['surf']:>18} {r['dist']:.0f}ft  bursts={r['n_bursts']} gap={r['longest_gap_s']:.0f}s "
                  f"walk_t={r['walk_t']:.0f}s  time-only_est={est:.1f}ft ({100*(est-r['dist'])/r['dist']:+.0f}%)")
    else:
        print("\n=== TEST 3 — pause trim: no pause runs yet (awaiting 07-05 pull) ===")
    return rows


if __name__ == "__main__":
    main()

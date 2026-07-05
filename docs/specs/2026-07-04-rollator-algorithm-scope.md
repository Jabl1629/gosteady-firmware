# Rollator Wheeled-Motion Algorithm — Scoping v0

> **Status:** scoping / pre-design · **Date:** 2026-07-04 · **Author:** algo + Claude
> **Data basis:** 16 recovered runs, `raw_sessions/2026-07-04-dt1-rollator/`
> (coord §C51 — the "lost" runs; all validate, ingested to `capture_2026-07-04.csv`).
> **Figures:** `algo/figures/rollator/{A,B,C}_*.png` (regen: `algo/venv/bin/python` the
> scratch EDA scripts). **Parity target (memo D10, protocol §1):** walker-cap metric
> parity — `steps`, `distance_ft`, `active_min`, `gait_speed_fts` — by DT-2 exit.

## TL;DR

1. **The frame-mount rollator signal is wheel-vibration-dominated, not step-impulse.**
   Empirically confirmed: 72–98 % of dynamic-accel energy is in the 10–45 Hz
   wheel band; the 0.7–3 Hz human-gait band holds ~0–7 %. **The walker's
   Schmitt step-peak detector has almost nothing to bite on here** — the
   protocol's premise ("lift-and-place does not transfer") is now measured, not
   assumed. See Fig A.
2. **This inverts the metric difficulty vs the walker.** For the walker, `steps`
   was the easy primitive and everything derived from it. For the rollator,
   `steps` is the *hardest* metric and `active_min` is nearly free.
3. **Recommended pivot:** build distance/speed from **wheel-vibration odometry**
   (amplitude scales with speed — Fig B), not step×stride. Treat `steps` as an
   open question requiring a decision (mount change, or redefine the metric).

## 1. What this dataset is (and its limits)

16 runs, indoor **polished concrete** (14) + a 2-run start of **outdoor
concrete**. Recovered off-device after the capture-page confirmation bug made
them look lost (they recorded fine; see coord §C51 correction). Honest caveats
that bound every conclusion below:

- **~3 of the 9 "normal" concrete runs are dead-air false starts** (`hp_rms`
  ≤ 0.003 g, `active`=0.00 — operator hit START, saw no ack, never walked).
  Usable walking runs: ~6 concrete + 2 outdoor + the maneuver runs.
- **No turns, no s-curve** (matrix rows 7–9 never captured) — zero data for
  the turn detector.
- **No carpet** (rows 27–39) — only 2 of 3 surfaces, and outdoor is n=2.
- **Single subject (S00, operator), single mount, no ground truth**
  (`actual_distance_ft` defaulted to intended; `manual_step_count` blank).

So this is a **feasibility probe, not a training set.** It's enough to pick a
direction and kill dead ends; it is not enough to fit or validate a shipping model.

## 2. What the signals show

Per-run features (`hp_rms` = RMS of 0.2 Hz-HP |a| in g; `active` = motion-gated
fraction; `pwheel`/`pgait` = fraction of energy in 10–45 / 0.7–3 Hz):

| run_type | n | hp_rms (g) | active | gyro_pk (dps) | pwheel | pgait |
|---|---|---|---|---|---|---|
| normal (walking, real) | ~8 | 0.04–0.16 | 0.6–0.83 | 10–33 | 0.77–0.98 | 0.00–0.07 |
| normal (dead-air start) | ~3 | ≤0.003 | 0.00 | 0–1 | — | — |
| brake_stop | 2 | 0.03–0.044 | 0.54–0.72 | 9–13 | 0.74–0.94 | ~0 |
| heavy_lean | 1 | 0.052 | 0.83 | 12 | 0.96 | ~0 |
| stationary_baseline | 1 | 0.0041 | 0.00 | 0 | — | — |
| park_brake_seated | 1 | 0.0011 | 0.00 | 1 | — | — |

Reads:

- **Motion vs still is trivially separable** (~10–40× in `hp_rms`, 4–5 orders in
  PSD — Fig A). `active_min` is essentially free and robust.
- **Wheel-vibration dominates** (Fig A): walking energy peaks 10–35 Hz, 3–5
  decades above the still floor. No clean gait-cadence peak — `fdom` in the
  0.6–3.5 Hz band is inconsistent (0.78→3.32 Hz, no run-to-run stability).
- **Speed lives in vibration amplitude** (Fig B): polished-concrete `hp_rms`
  rises slow→normal→fast (0.041 → ~0.06 → 0.126 g). This is the distance/speed
  enabler.
- **Surface is a confound** (Fig B): outdoor "normal" (`hp_rms` 0.13–0.16)
  vibrates as hard as polished "fast." Amplitude alone conflates speed with
  roughness → a surface estimate is required to use vibration as an odometer.
- **Events are visible** (Fig C): brake_stop shows a clean mid-run
  motion→still→motion gap; seated/parked is flat. Turn (gyro) is untested —
  straight-run `gyro_pk` is only 10–33 dps, so a real 90° turn should stand out,
  but we have no turn data to confirm.

## 3. Metric-by-metric feasibility (vs the parity targets)

| Metric | Verdict | Approach | Blockers |
|---|---|---|---|
| `active_min` | **Feasible now** | Reuse walker motion gate (σ of HP-accel, Schmitt hysteresis). Separation is huge. | None. Port + validate. |
| `distance_ft` | **Plausible, needs data** | **Vibration odometry**: map surface-normalized `hp_rms` (and/or wheel-band spectral features) → speed, integrate over active time. Replaces step×stride entirely. | Needs speed×surface matrix + measured ground-truth distance. Cross-rollator/wheel/tire generalization unknown. |
| `gait_speed_fts` | **Follows distance** | `distance_ft` ÷ active (moving) time — same shape as walker §C46, but "moving time" from the motion gate, not a peak train. | Inherits the distance error floor + the surface confound. |
| `steps` | **DROPPED (decision 2026-07-04)** | Not a rollator metric — the cupholder mount can't see gait (pgait ≈ 0). Report active-time + distance + gait speed instead. | — resolved. |

Rollator-specific (no walker analog, all **unvalidated — need the missing runs**):

- **Turn detection** — integrate gyro-Z; 90° turns should be obvious. *Needs rows 8–9.*
- **Brake events** — mid-session motion-gate transitions (Fig C shows the signature). *2 runs; workable.*
- **Seated / parked** — motion gate + gravity-vector tilt shift. *1 run each; clean.*
- **Heavy lean** — expected as a static tilt/load offset, but the one run shows no
  obvious inclination change (mount angle dominates). *Weak signal; low priority.*

### 3a. Distance estimator v0 — first result (2026-07-04)

Prototyped surface-normalized **vibration odometry**: distance ≈ `k` · Σ(active
`|a|_HP` RMS · dt) — integrate a vibration speed-proxy over motion-gated windows.
Feature = `vib_int`; ground truth = the **taped course distance** — start/end tape
lines the front wheels traverse, i.e. a real measured distance (the POST-WALK notes
were empty, but the distance itself was set by tape and equals `intended`). Fig D.

Recording-hygiene check: the walking is one contiguous burst in most runs
(`main_frac` ≈ 0.96–1.0), and trimming to the longest clean segment did *not* improve
the fit (22 % vs 16 %) — so the recordings map cleanly to the taped distance; the
taped value is trustworthy GT, not a guess. (Naturally-fragmented runs — brake_stop,
the very-slow run — are the only multi-burst cases, as expected.)

- **The integral scales ~linearly with distance.** Polished concrete, normal
  speed: 10 ft → `vib_int` 0.75 (0.77, 0.74 — tight); 20 ft → 1.58. **Ratio 2.09×
  for a 2× distance.** The core hypothesis holds.
- **Single-surface first cut ≈ 16 % MAPE** (one constant `k`, linear, in-sample,
  vs approximate GT) — *comparable to the walker's 22.4 %*. Errors concentrate at
  the speed extremes (slow −29 %, fast +26 %) → the speed↔vibration map is
  **nonlinear**; a fitted curve will tighten it.
- **Surface normalization is mandatory, not optional.** The polished-calibrated
  `k` applied to outdoor gives **+86 – 130 %** error (outdoor vibrates ~2.5× harder
  at equal speed). Amplitude odometry MUST be gated on a surface estimate.
- **No surface-robust frequency shortcut.** Hunted for a wheel-rotation peak that
  tracks speed independent of surface (would sidestep the confound) — the low-band
  peak is inconsistent and the spectrum is dominated by a fixed ~22 Hz frame
  resonance. So the odometer stays amplitude-based + surface-classified, not
  rotation-counting.

**Verdict: a reasonable distance estimator is achievable.** First-cut single-surface
accuracy is already walker-comparable, measured against the taped distances. Three
things convert it to production: (1) a **surface classifier** (R2 — needs ≥3 surfaces
incl. carpet); (2) a **nonlinear speed↔vibration calibration** (fix the slow/fast
tails); (3) **more coverage / n** — the 16 % is in-sample on n=6 over a 10–20 ft
range; more runs per (surface × speed × distance) cell + a 40 ft long run give a
trustworthy out-of-sample number and pin the nonlinear curve. GT is *not* the gap —
the taped courses already provide it.

Note: the "normal"-speed runs averaged ~0.8–1.0 ft/s (10 ft over ~12 s of continuous
push) — slower than natural gait. Worth confirming the intended bench push pace; it
shifts where "slow/normal/fast" sit on the speed↔vibration curve.

### 3b. Surface-normalized estimator + LOO evaluation (2026-07-05)

Built `algo/rollator_distance_v0.py` and evaluated **leave-one-out** (out-of-sample)
vs the taped distances. Roughness = per-run **spectral flatness** of the wheel
vibration — separates our two surfaces (polished 0.29 vs outdoor 0.59) while staying
~speed-independent (speed-corr 0.27; probe in `rollator_roughness_probe`). Model:
`distance = m · (vib_int / flatness)`.

| model | LOO MAPE | polished | outdoor |
|---|---|---|---|
| naive — single constant, no surface | 41 % | 26 % | 87 % |
| oracle — true surface label picks the constant | 20 % | 20 % | 22 % |
| **flatness-normalized — continuous, no label** | **17 %** | 20 % | **7.5 %** |

Surface normalization **~halves** distance error (41→17 %) and rescues the outdoor
runs (87→7.5 %). The continuous flatness feature **matches/edges the oracle** — no
surface *label* needed, and it pools all runs into one constant, beating a discrete
2-class classifier that has only 1–2 outdoor points to calibrate. 17 % is
walker-comparable (22 %). Fig F.

**Confidence: mechanism demonstrated, NOT validated.** n=8 over 2 surfaces; the
flatness feature was chosen on these same runs; all speeds 0.7–1.2 ft/s; distances
only 10/20 ft; the single-constant `speed ∝ amp/flatness` collapse is a 2-point
coincidence-risk. **The decisive test is a real 3rd surface (carpet):** does flatness
still track its roughness, and does the one constant still hold?

## 4. Proposed development arc

- **R0 — Active-time primitive (now, ~days).** Port/validate the motion gate on
  the rollator set; ship `active_min`. Zero new research; immediate parity on one
  of four metrics. De-risks the firmware plumbing (product-split payload already
  emits `active_min`, §C49).
- **R1 — Vibration→speed/distance model (core research).** Fit surface-normalized
  `hp_rms`/spectral features → speed on the full speed×surface matrix; integrate to
  distance; derive gait speed. **Gated on data** (§5). Prototype in `algo/`
  mirroring the walker's loader/evaluator/LOO harness.
- **R2 — Surface classifier.** Vibration-amplitude/spectral R to pick the R1
  normalization (analogous to the walker roughness classifier, but here it
  *de-confounds the odometer* rather than selecting stride coeffs). Needs ≥3
  surfaces incl. carpet.
- **R3 — Event detectors.** Turn (gyro-Z), brake (gate transitions), seated/parked
  (gate+tilt). Gated on the missing maneuver runs.
- **R4 — `steps` decision + (if pursued) mount experiment.** Resolve the open
  question below; if steps stays in scope, run the side-rail mount comparison.

## 5. Data gaps → next capture priorities

To move past feasibility, the next capture day should prioritize:

1. **Coverage & n** (the real blocker — distance GT is already in hand from the
   taped courses): more runs per (surface × speed × distance) cell for an
   *out-of-sample* number; a wider distance range incl. a **40 ft long run** to test
   linearity beyond 2×; enough per-speed reps to fit the nonlinear speed↔vibration
   curve. Keep taping distances as before. *Optional* nicety: a stopwatch on one
   marked segment per run as a direct speed anchor (distance ÷ moving-time already
   gives per-run average speed, so this is refinement, not a blocker).
2. **Complete the matrix**: turns + s-curve (rows 7–9), **carpet** (rows 27–39),
   fill out outdoor. More **speed reps per surface** (the R1 fit is a speed×surface
   surface; n=1 "fast" today).
3. **Mount discipline**: finalize and record the accessory-platform mount rigidly
   (protocol §2 flags it as an open item — mount compliance scales the vibration
   amplitude the odometer reads, so it changes every number here).
4. Multiple subjects / real user weights (currently S00 only).

## 6. Open decisions (need your call)

- **D1 — Is `steps` a hard rollator requirement?** **RESOLVED 2026-07-04: dropped.**
  Rollator metric set is `active_min` + `distance_ft` + `gait_speed_fts`. This also
  retires the side-rail-mount question that only existed to chase steps.
- **D2 — Mount:** with steps dropped, no steps-driven A/B is needed — **commit to the
  accessory-platform (cupholder) position.** Still must be *finalized and rigidly
  fixed* (protocol §2 open item): mount compliance directly scales the vibration
  amplitude the odometer depends on, so a soft/variable mount would break the
  surface calibration.
- **D3 — Sequencing:** **RESOLVED: distance-first.** R0 (`active_min`) in parallel;
  R1 (distance) is the headline; events (R3) follow.

## 7. Risks

- **Generalization**: vibration→speed likely varies with rollator model, wheel
  material/diameter, tire wear, load, and mount rigidity. A per-mount/per-model
  calibration may be unavoidable — needs to be tested early on ≥2 rollators.
- **Data scarcity**: single subject/mount/mostly-one-surface; conclusions are
  directional. Don't fit a shipping model on this set.
- **Firmware cost**: R1 wheel-band features may need higher sample rate / on-device
  spectral work vs the walker's cheap time-domain peak detector — watch the RAM/CPU
  budget on the rollator build (snippets already off for RAM, §C49).

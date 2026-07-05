# Rollator Distance Estimator — Firmware Port Scope

> **Status:** scope / pre-implementation · **Date:** 2026-07-05
> **Depends on:** `docs/specs/2026-07-04-rollator-algorithm-scope.md` (algorithm +
> validation, §3e/§3f). **Do not implement until this scope is agreed.**
> Architecture facts below cite the current firmware (map: 2026-07-05 Explore pass).

## 1. What ships

Add **`distance_ft`** and **`gait_speed_fts`** to the rollator activity uplink,
computed on-device by a **continuous flat-norm vibration odometer** (no surface
classifier). Validated on-bench at **~26–29 % MAPE** (walker-parity ~22 %), 3
surfaces, variable speed, out-of-sample. `active_min` already ships and is unchanged.

The estimator (validated, `algo/rollator_surface_classifier.py`):
```
distance = m · Σ_active( wheelband_RMS_window · dt ) / flatness
```
- **motion gate** = Schmitt hysteresis on the **wheel band (10–44 Hz)** envelope
  (rolls → wheel vibration; leaning/fidget → none, so it's excluded).
- **flatness** = spectral flatness of the active vibration — a speed-independent
  roughness read that de-confounds amplitude (a rougher surface buzzes harder at
  equal speed). **Cheap time-domain proxies fail** (crest-norm 58 %, kurt-norm 69 %
  vs flatness 29 %) — flatness is required.
- **m** = one global calibration constant. No per-surface curves, no classifier.

## 2. Where it plugs into the firmware

| Concern | Current firmware | Port |
|---|---|---|
| Sample feed | `session.c:340` → `gs_pipeline_step(mag_g)` @100 Hz (gravity-norm mag) | unchanged |
| Finalize | `session.c:386` → `gs_pipeline_finalize(outputs)` at stop | add rollator backend call |
| Outputs | `gs_pipeline_outputs` (12 fields, `gs_pipeline.h:56`) | add `rollator_distance_ft`, reuse `gait_speed_fts` |
| Walker distance path | step detector → stride regression (`gs_pipeline.c:199`) | **not used** for rollator (no steps) — new parallel path |
| Roughness | time-domain inter-peak RMS (`gs_roughness.c`); **no FFT in `src/algo/`** | **new spectral-flatness feature** (filter-bank, §4b) |
| Motion gate | broadband σ Schmitt (`gs_motion_gate.c`, enter .01/exit .005 g/2 s hold) | **new wheel-band variant** (§4a) |
| Rollator payload | `active_min` only; distance/gait/roughness compile-gated OUT (`cloud.c:1081`) | add `distance_ft` + `gait_speed_fts` to the rollator branch |
| Calibration | compile-time `#define`/const arrays (`gosteady_algo_params.h`), gen by `export_c_header.py`; **no runtime path** | add `GS_ROLL_*` consts; same generator |

**Architecturally:** a compile-gated rollator distance backend
(`gs_rollator_distance.c`, invoked from the pipeline under
`CONFIG_GOSTEADY_PRODUCT_ROLLATOR`), parallel to the walker's step path. It is
**fully streaming** (per-window accumulators) — it does **not** need the walker's
60 KB `mag_lp_buf`/`motion_mask_buf` batch buffer, so its RAM cost is small (a few
biquads + accumulators). Good news for the ~60 %-RAM rollator build.

## 3. Firmware changes, module by module

**a. Wheel-band motion gate.** New 10–44 Hz bandpass (biquad, reuse `gs_filters`)
feeding a Schmitt gate (enter/exit/2 s-hold, params TBD from bench). Rationale: the
current broadband gate would count a lean/frame-load as travel; the wheel-band gate
correctly rejects it (validated — the lean-during-pause run leaked no distance).
The existing broadband gate stays for `active_min` (walker + rollator) unless we
decide to unify.

**b. Spectral flatness via filter bank (no FFT).** Compute band energies in ~8–16
log-spaced bands across the active vibration (streaming biquad energy accumulators),
then `flatness = geomean(E_k)/mean(E_k)`. Verified: filter-bank resolution ≈ full FFT
(16-band within ~1 pp of Welch). **Port task:** pin the exact band range + windowing
that reproduces the harness's 29 % (a naive 3–45 Hz single-window recipe lands ~38 %
— the recipe matters ~9 pp), with a host-test acceptance check (C flatness matches
Python within tolerance on fixtures). **This is the one genuinely new signal-processing
block** — everything else reuses existing infra.

**c. Flat-norm distance backend** (`gs_rollator_distance.c`): accumulate
`Σ_active(wheelband_RMS_window·dt)`, divide by per-session flatness, × `m`. Emit
`distance_ft`; `gait_speed_fts = distance_ft / walking_time_s` (reuse the pipeline's
existing `walking_time_s`, `gs_pipeline.c:208`).

**d. Pipeline integration:** call the rollator backend from `gs_pipeline_finalize`
under the product flag; add its outputs to `gs_pipeline_outputs`.

**e. Cloud contract:** add `distance_ft` + `gait_speed_fts` to the rollator branch
(`cloud.c:1081–1107`). Cloud-side validators + activity-processor must accept these
for `device_type:"rollator_platform"` (coordinate via the contract doc / coord entry
— they're currently walker-only fields). **Confidence gating:** emit `distance_ft`
only when valid (≥ N s active, flatness in range, not saturated); otherwise omit it
and ship `active_min` alone — never a confidently-wrong number.

**f. Calibration:** `GS_ROLL_DIST_M` + wheel-band coeffs + flatness band params as
compile-time consts via `export_c_header.py`. **Flag for decision:** the walker uses
compile-time coeffs with no runtime recalibration; if per-unit/per-rollator
calibration is needed (see §5 risk), we add an NV-storage load path — but that's a
bigger change and I'd defer it until the generalization test says whether it's needed.

**g. Host tests:** generate rollator fixtures from `raw_sessions/2026-07-0*-dt1-rollator`
via `export_reference_vectors.py`; add a replay test (`test_rollator_distance`) that
walks C vs Python per-window and checks `distance_ft`/`flatness`/gate within tolerance
(mirrors `test_main.c`). Gate the port on host-suite green + a bench re-pull matching
the Python numbers.

## 4. On-device cost

- **CPU:** wheel-band biquad + ~8–16 band biquads + per-window RMS/energy — all
  streaming, O(bands) per sample. Cheap on the M33+FPU. No FFT.
- **RAM:** streaming path, no session buffer needed (~hundreds of bytes of state).
  Should fit the ~40 % headroom easily — **confirm with a pristine build .config diff**.
- **Flash:** one small module + coeffs. Negligible.

## 5. Risks / open decisions (resolve before or during port)

1. **Generalization (the #1 risk).** `m`, the flatness↔roughness mapping, and the gate
   thresholds are all from **one rollator / mount / subject**. Wheel size/material/tire
   wear, mount rigidity, and user weight will shift them. **Recommend a second-rollator
   (or re-mount) capture before shipping** — it decides whether one compile-time `m`
   suffices or we need the NV-storage per-unit calibration path (§3f). This is the
   gating question for the whole port.
2. **Flatness recipe fidelity.** The 29 % depends on the exact flatness recipe; the
   C filter-bank must reproduce it (host-test acceptance). Budget a tuning pass.
3. **`active_min` gate divergence.** Rollator distance uses a *wheel-band* gate;
   `active_min` uses the *broadband* gate. Decide: keep two gates (distance excludes
   leans, active_min includes them — arguably correct, since a lean IS activity) or
   unify. Leaning toward keeping both (they measure different things).
4. **Accuracy framing.** Ship `distance_ft` as a within-resident **trend** (±~26 %),
   not a precise odometer — mirror the walker gait-metric framing in the contract/UX.

## 6. Suggested phasing

- **P0 (no firmware):** second-rollator generalization capture → decide compile-time
  vs NV calibration (§5.1). *Gate for everything below.*
- **P1:** `gs_rollator_distance.c` + wheel-band gate + filter-bank flatness, host-test
  parity vs Python (the 29 % recipe). No cloud yet.
- **P2:** pipeline + payload integration behind `PRODUCT_ROLLATOR`; cloud contract +
  processor accept the new rollator fields; confidence gating.
- **P3:** bench validation (reflash rollator build, capture, confirm on-device
  `distance_ft` matches the harness within tolerance), then field.

## 7. Non-goals

Surface classifier (ruled out — continuous flat-norm, §3f). Wheel-rotation odometry
(ruled out, §3f). Steps (dropped for rollator). Runtime NV calibration (deferred to
§5.1 outcome).

## 8. Implementation status (2026-07-05) + flash/test runbook

**P1 DONE — module + host-parity validated.** `src/algo/gs_rollator_distance.{c,h}`
+ `gs_rollator_params.h` (coeffs) + `algo/rollator_distance_ref.py` (golden
reference, 31.7 % LOO). Host parity (`tests/host/test_rollator_distance.c`) across
**all 44** valid 2026-07-04+05 walking sessions: distance & flatness max rel-err
**0.02 %**, gate windows identical, valid 44/44. Two bugs fixed en route: the CMSIS
biquad sign convention (a1,a2 negated vs scipy — matches `gs_biquad`) and window RMS
= population std (mean-subtracted, = `np.std`; raw RMS ran ~10 % high on the HP band).

**P2 DONE — integrated + builds.** Wired into `session.c` (per-session reset /
per-sample step / finalize) + the activity payload (`cloud.c` emits `distance_ft` +
`gait_speed_fts`, confidence-gated `isfinite`→omit) + `CMakeLists`, **all under
`#if CONFIG_GOSTEADY_PRODUCT_ROLLATOR`** (walker builds byte-identical — every
rollator ref verified compile-gated). Rollator capture image builds clean
(`build_rollator_dist/merged.hex`, RAM 16 %). A uart0 `ROLL_DIST` line at session
stop gives the on-device readout **without needing cloud**.

**BLOCKED — flash.** The external J-Link Mini won't open a debug session (`nrfjprog`
→ JLinkARM error −256 at DLL-open, identical for nRF91 and nRF53, persists after
clearing stale holders; `--ids` still lists 802006700). Needs a **physical re-plug
of the J-Link Mini** (and SW2 = nRF91) — can't be done headless. Also: distance
needs *real motion*, so on-device accuracy needs the rollator **pushed** (a bench
session only exercises the code path → `valid=0`).

### Flash + test runbook (when the J-Link is back)
```
# 1. Re-plug the J-Link Mini USB; SW2 = nRF91. Verify it opens:
nrfjprog -f NRF91 --snr 802006700 --deviceversion      # expect a version, not -256
# 2. Flash the rollator distance image:
nrfjprog -f NRF91 --recover --program build_rollator_dist/merged.hex --verify --reset --snr 802006700
# 3. Watch uart0:
screen /dev/cu.usbmodem*102 115200
# 4. Record a session while PUSHING the rollator (capture_rollator.html BLE, or
#    tools/control.py over uart1): START -> walk -> STOP.
# 5. At STOP, uart0 prints:  ROLL_DIST valid=1 dist_ft=.. gait_fts=.. flat=.. walk_s=.. nact=..
#    (stationary session -> valid=0, i.e. no travel counted — gating works.)
# 6. Cross-check: pull the .dat, run the host C + golden reference on it:
tools/pull_sessions.py --port /dev/cu.usbmodem1105 --out /tmp/verify
cc -std=c99 -O2 -I src/algo tests/host/test_rollator_distance.c src/algo/gs_rollator_distance.c src/algo/gs_filters.c -lm -o /tmp/roll_test
/tmp/roll_test /tmp/verify/<uuid>.dat      # should match the on-device ROLL_DIST line
# 7. Restore the original capture image if needed:
nrfjprog -f NRF91 --recover --program build_rollator_bench/merged.hex --verify --reset --snr 802006700
```

**Not yet done (post-flash / follow-up):** on-device flash + a pushed-rollator
accuracy check; the cloud rollator build (`prj_rollator_cloud.conf`) to verify the
payload path end-to-end (pre-existing RAM-overflow there, §C49 — the payload edit
itself mirrors the walker's `isfinite`/`snprintf` and is low-risk); the
generalization capture (§5.1) that gates compile-time vs NV calibration.

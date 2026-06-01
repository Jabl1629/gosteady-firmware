# Pre-activation low-power ("shipping") mode — design spec

> **Status:** PROPOSED — not yet implemented. Spec only.
> **Author:** Claude (firmware session, 2026-05-31)
> **Motivates:** ship/store a pre-activation cap for months with no pull-tab and
> still hand the patient a near-full battery.
> **Related:** GOSTEADY_CONTEXT.md (§C38 battery model), coord §C39 (D2C device
> staging), `prj_pilot.conf` / `CONFIG_GOSTEADY_LOW_POWER`,
> `CONFIG_GOSTEADY_SESSION_LED`.

---

## 1. Problem

A pre-activation device today is **not** a low-power standby — it is a fully
active cellular device that merely declines to capture sessions. Empirically
(GS0000000001, coord §C39) a pre-activation cap:

- publishes a **full LTE-M heartbeat every hour**, and
- spins the **100 Hz sampler thread** continuously (idle spin never gated).

Per the §C38 model that is **~340–410 mAh/month in storage (~27 % of a 1350 mAh
cell)** — heartbeat ~180 + sampler-spin app core ~150–220. A cap on a shelf for
2–4 months could be dead before the patient ever claims it. None of that cost is
the LED or the motion sensor.

**Target:** ~**50 mAh/month** pre-activation (≈ 28-month idle shelf life before
self-discharge), while keeping the device responsive: a **blue blink on pickup**
("alive, waiting to be set up") and prompt activation when the user sets it up.

---

## 2. Behavior state machine

| State | Sampler | BMI270 | Heartbeat | Modem | LED |
|---|---|---|---|---|---|
| **PRE_ACTIVATION — idle** | blocked | suspended (~0.5 µA) | safety-net only (~24 h) | PSM-registered (~5 µA) | dark |
| **PRE_ACTIVATION — motion** | blocked | not resumed (no confirm) | motion-triggered connect (rate-limited) | wakes for the connect | **blue blink** (rate-limited) |
| **ACTIVATED — idle** | blocked | suspended | 1 h | PSM-registered | dark |
| **ACTIVATED — recording** | running 100 Hz | active | 1 h | PSM-registered | **green solid** (SESSION_LED) |

Transition **PRE → ACTIVATED** fires when the `activate` cmd is received
(`cloud.c::handle_activate_cmd` → `gosteady_activation_apply`). All three
mechanisms below read `gosteady_activation_is_activated()` each loop, so no new
callback is needed — they reconfigure on the next iteration.

LED contract for the field/pilot build: **blue = pre-activation "pick me up to
set up"; green = recording; nothing at rest.** (Idle purple blink stays off.)

---

## 3. Mechanism A — gated sampler (the big battery term)

**Today** (`main.c::sampler_entry`): `while (1) { active = is_active(); if (active) fetch+append; k_msleep(10); }` — wakes the app core 100×/s forever, even with no session, defeating `CONFIG_PM`.

**Proposed:** the sampler **blocks** until a session starts, runs 100 Hz only while active, then blocks again.

```
while (1) {
    gosteady_session_wait_for_start(K_FOREVER);     // NEW: blocks here when idle
    /* idle→active: resume BMI270 + 25 ms settle + 2 prime + drop-4 warm-up
       (unchanged from today's transition handler) */
    while (gosteady_session_is_active()) {
        fetch + append;
        k_msleep(10);                                // 100 Hz
    }
    bmi270_set_active(false);                         // active→idle: suspend, loop back to block
}
```

**Signaling (lives in `session.c`, next to `s_active`):** add a sampler-wake
signal raised inside `gosteady_session_start()` *after* `s_active=true` and the
existing writer `start_signal` handshake:

- `static K_SEM_DEFINE(sampler_start_sem, 0, 1);`
- `int gosteady_session_wait_for_start(k_timeout_t)` → `k_sem_take`.
- `gosteady_session_start()` → `k_sem_give(&sampler_start_sem)` on success.

This keeps both start paths uniform: the **auto-start coordinator** (field) and
**SW0/BLE** (bench) both call `gosteady_session_start`, so both wake the sampler.

**Race surface (this is the deferred-for-good-reason part).** Touches the
documented session races: cross-session sample leak (fixed via the synchronous
start handshake), `-EBADF`/HardFault on STOP (fixed by setting `s_active=false`
*before* raising `stop_signal`), and the `stop_done_sem` reset. The gated design
**preserves** all three — it only changes how the sampler *waits* (sem vs spin),
not the start/stop ordering. The `sampler_start_sem` give must come **after**
`s_active=true` so the first sampler iteration sees `is_active()==true`.

**Scope decision (open):** make the gating **unconditional** (strictly better,
one code path, benefits bench too) vs **gate behind `CONFIG_GOSTEADY_LOW_POWER`**
(conservative — leaves the bench spinning sampler untouched while we validate).
Recommend: implement unconditional + the test plan in §7; fall back to flag-gated
if the soak surfaces any regression.

---

## 4. Mechanism B — blue-on-motion (pre-activation indicator)

**Today** (`main.c::auto_start_entry`): every motion event resumes the BMI270,
runs a 500 ms confirmation, then calls `session_start` — which returns `-EACCES`
in pre-activation. So pre-activation pays the BMI270 confirm cost on every bump
and shows nothing.

**Proposed:** branch at the top of the motion handler, *before* the BMI270 resume:

```
k_sem_take(&motion_event_sem, K_FOREVER);
if (!gosteady_activation_is_activated()) {
    blue_blink_burst();                 // rate-limited; ~2 flashes, no BMI270 needed
    request_preactivation_connect();    // rate-limited cellular check (Mechanism C)
    k_sem_reset(&motion_event_sem);     // debounce
    continue;
}
/* activated: existing confirm → session_start → led_set_recording(true) path */
```

- **No BMI270 resume in pre-activation** — saves the confirm-window power; raw
  ADXL367 motion is enough for a "pick me up" blink.
- **`blue_blink_burst()`**: e.g. 2× (80 ms on / 120 ms off) on `led_blue`, then
  off. Driven in the auto-start thread context. New helper in `main.c`.
- **Rate-limit the blink** to ≥ ~3 s between bursts (avoid a strobe under
  sustained handling). Track `last_blink_uptime`.

LED matrix is then exactly the §2 table. (Note: this finally implements the
"pre-activation indicator" the D2C spec assumed but field builds never had — see
coord §C39.5.)

---

## 5. Mechanism C — slowed / motion-triggered heartbeat

**Today** (`cloud.c::heartbeat_thread_fn`): `while (1) { publish; k_sleep(HEARTBEAT_INTERVAL=1h); }`, independent of activation.

**Why pre-activation must still connect *sometimes*:** the `activate` cmd is
delivered only when the device **connects** (queued at the broker with
`CLEAN_SESSION=n` + re-published by the connection-coordinator on the AWS IoT
`CONNECTED` event, coord §C24). A fully silent device never activates.

**Proposed:** two changes to the heartbeat loop:

1. **Activation-dependent interval.** `interval = activated ? 1 h : PREACT_SAFETY_INTERVAL (~24 h)`. Re-read each loop, so the loop after activation uses 1 h.
2. **Motion-triggered connect in pre-activation.** The loop waits on **either**
   the safety-net timer **or** a `preact_connect_sem` (given by Mechanism B,
   rate-limited to ≥ ~15 min between fires), whichever first:
   ```
   if (activated)  k_sleep(K_HOURS(1));
   else            k_sem_take(&preact_connect_sem, K_HOURS(PREACT_SAFETY_HOURS));  // wakes early on motion
   connect_publish_disconnect(...);   // delivers any pending activate cmd in the 1.5 s linger
   ```

**UX this produces:** user claims in the app → cloud queues `activate` → user
**picks up / mounts the cap** → motion → device connects within seconds →
receives `activate` → activates (green LED on first walk). If never moved, the
24 h safety net still activates it within a day. Matches the §C39 runbook's
"power-cycle after claim" but makes a *motion* nudge sufficient too.

**Modem:** stay **PSM-registered** between connects (~5 µA ≈ 3.6 mAh/month —
negligible) rather than `CFUN=0`/re-attach-each-time. PSM TAU (3 h, already
configured) keeps registration warm so the motion-triggered connect is fast.

---

## 6. Kconfig

```
config GOSTEADY_PREACT_LOWPOWER
    bool "Pre-activation low-power (shipping) mode"
    depends on GOSTEADY_CLOUD_ENABLE
    default n          # set in prj_pilot.conf
    help
      Gates the pre-activation shipping behaviors (blue-on-motion indicator,
      slowed + motion-triggered heartbeat). The gated sampler is independent
      (see GOSTEADY_LOW_POWER / unconditional decision in the spec §3).
```

Tunables (Kconfig ints, with the recommended defaults):
- `GOSTEADY_PREACT_HEARTBEAT_HOURS` = **24** (safety-net cadence)
- `GOSTEADY_PREACT_MOTION_CONNECT_MIN_S` = **900** (15 min min between motion connects)
- blue blink: count **2**, on **80 ms**, off **120 ms**, min gap **3 s**

---

## 7. Estimated power (recomputed with this mode)

1350 mAh cell; §C38 model currents (estimates — the GS9999999998 discharge run +
the nPM1300 `IBAT` instrument calibrate the idle floor and LED).

| Pre-activation, 1-month storage (no motion) | mAh/mo |
|---|---|
| App core idle, **sampler gated** (~50 µA) | ~36 |
| Safety-net heartbeat (24 h → ~30/mo × 0.25 mAh) | ~7.5 |
| Modem PSM-registered (~5 µA) + periodic TAU | ~4 |
| ADXL367 armed (~1 µA) + BMI270 suspended | ~1 |
| **Total** | **~48 mAh/mo (~3.5 %)** |

vs **~340–410 mAh/mo today** → **~7–8× better**; ~28-month idle shelf life before
LiPo self-discharge (~2–3 %/mo) becomes the limit → **comfortably 6–12+ months**
on a shelf with plenty left for the patient.

| 2 h of handling (motion) | mAh |
|---|---|
| Blue blink bursts (rate-limited, ~2 mA) | ~0.5–2 |
| Motion-triggered connects (rate-limited ~4–8 × 0.25 mAh) | ~1–2 |
| Slightly elevated app core | ~0.5 |
| **Total** | **~3–5 mAh (~0.3 %)** |

---

## 8. Implementation map (per file)

| File | Change |
|---|---|
| `Kconfig` | add `GOSTEADY_PREACT_LOWPOWER` + tunables |
| `prj_pilot.conf` | set `CONFIG_GOSTEADY_PREACT_LOWPOWER=y` |
| `src/session.c` | add `sampler_start_sem` + `gosteady_session_wait_for_start()`; give the sem in `gosteady_session_start` after `s_active=true` |
| `src/session.h` | declare `gosteady_session_wait_for_start()` |
| `src/main.c` (`sampler_entry`) | block on `wait_for_start`; remove the 100 Hz idle spin |
| `src/main.c` (`auto_start_entry`) | pre-activation branch: `blue_blink_burst()` + `request_preactivation_connect()` + skip confirm |
| `src/main.c` | `blue_blink_burst()` helper + blink rate-limit |
| `src/cloud.c` (`heartbeat_thread_fn`) | activation-dependent interval; wait on (timer OR `preact_connect_sem`) |
| `src/cloud.c` | expose `gosteady_cloud_request_preact_connect()` giving `preact_connect_sem` (called from `auto_start_entry`) |
| `src/version.h` | bump (e.g. `0.14.0-shipmode`) |

---

## 9. Risks & test plan

1. **Sampler gating (highest risk).** Soak many auto-start/auto-stop + SW0/BLE
   start/stop cycles; assert: no lost leading samples, no `-EBADF`/HardFault on
   stop, BMI270 actually suspends between sessions (verify with the nPM1300
   `IBAT` instrument: idle current drops vs the spinning build), algo outputs
   unchanged vs the 31/31 host suite on identical input.
2. **Motion-connect rate-limit** — verify sustained handling does **not** spam
   cellular (iBasis trial budget); count connects over a 2-min shake test.
3. **Activation promptness** — after claim+activate: blue stops, green works on
   first walk, heartbeat switches to 1 h, capture starts. Test both the
   motion-trigger path and the 24 h safety-net path (synthetically).
4. **PSM wake** — confirm the modem reliably wakes from PSM for a
   motion-triggered connect (watch `rrc=connected` after a long PSM sleep).
5. **Measure, don't model** — use the nPM1300 `IBAT` log (LOW_POWER) to replace
   the §7 estimates for the blue LED and the gated-idle floor.

---

## 10. Open decisions (need a call before implementing)

- **Sampler gating: unconditional vs `LOW_POWER`-gated** (§3). Lean unconditional.
- **Pre-activation safety-net interval:** 24 h (proposed) vs 12 h vs 48 h.
- **Motion-connect rate-limit:** 15 min (proposed) vs 30 min.
- **Modem in storage:** PSM-registered (proposed) vs `CFUN=0` deep-off.
- **Blue pattern:** brief 2-flash burst (proposed) vs slow single pulse.

## 11. Cloud-coordination note

A pre-activation safety-net heartbeat of ~24 h means a shelf device is quiet for
up to a day. The future **Phase 1C offline detector** (`lastSeen > 2 h`) MUST
scope to `active_monitoring` devices only — it must **not** alarm on
`ready_to_provision`/`provisioned` units, or every boxed cap trips it. (No
conflict today; 1C isn't built. Flag for whoever builds it — sibling to the
§C39 staging note.)

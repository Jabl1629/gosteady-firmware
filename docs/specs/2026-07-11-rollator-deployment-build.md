# Rollator deployment build — spec (DT-3)

> **Date:** 2026-07-11 (updated 2026-07-12) | **Status:** 🟡 build + **shake-to-
> activate VALIDATED on GS0002000001** (2026-07-12, coord §C58 — dark → shake →
> BLUE wake window → green confirm → `active_monitoring`; pre-activation gate
> holds, no green pre-claim). **Still owed:** rolling/false-trigger + on-battery
> discharge (§5 checklist). | **Owner:** firmware
> **Context:** the first real prod activation (GS0002000001, coord §C57 follow-up)
> ran on the **bench** build `prj_rollator_cloud`. This spec covers the **deployment**
> build (`prj_rollator_pilot`) — what changes, what's already proven, and the one
> thing that must be validated on real rollator hardware before any unit ships.
>
> **Sources of truth (don't duplicate):** [`docs/build-configurations.md`](../build-configurations.md)
> (the overlay×symbol matrix) · [`docs/specs/preactivation-lowpower-mode.md`](preactivation-lowpower-mode.md)
> (the PREACT shipping-mode design).

---

## 1. What the deployment build is

`prj_rollator_pilot.conf` → version `rol-0.1.0-ww`, `device_type:"rollator_platform"`.
It is `prj_rollator_cloud` **plus four symbols** (build-configurations.md §3):

| Symbol | cloud (bench) | **pilot (deploy)** | Effect |
|---|:--:|:--:|---|
| `FIELD_MODE` | — | ✅ | **Dark posture**: uart0 console + uart1 dump + idle/heartbeat LEDs all OFF |
| `LOW_POWER` | — | ✅ | Battery cadences relaxed (fuel-gauge 5 s→60 s, reporter poll relaxed) |
| `SESSION_LED` | — | ✅ | Green recording LED still lights under FIELD_MODE (the only normal-op LED) |
| `PREACT_LOWPOWER` | — | ✅ | **Pre-activation gate** + shake→wake-window→activate shipping mode, 24 h safety-net heartbeat, transit back-off |

`prj_rollator_field.conf` is the same minus `LOW_POWER`/`SESSION_LED`/`PREACT_LOWPOWER`
(dark, but no battery/shipping mode) — a strict subset, kept for a non-shipping dark deploy.

## 2. The behavioral change that matters — the pre-activation gate

On the **cloud** build there is **no pre-activation gate** (`main.c` gates it under
`#if CONFIG_GOSTEADY_PREACT_LOWPOWER`): any motion auto-starts a session and lights
green **regardless of activation**. That's fine for a bench test, wrong for a shipped
unit — a device recorded before the household claims it, and any pre-claim upload is
dropped cloud-side for having no assignment (observed on GS0002000001: green-on-pickup
before the claim).

On the **pilot** build the gate is active: pre-activation, motion runs a **blue-pulse
wake window** (a stay-connected activation attempt), **not** a session. The device
only records once the claim's activate cmd lands. Post-activation it behaves like the
cloud build (motion auto-start → green → record → upload).

Also relevant (both builds, already fixed): the activate cmd_id is echoed in full
(gosteady `0dc3523`) and an activate now triggers an **immediate** heartbeat ack
(`5324889`) — so `provisioned→active_monitoring` completes in seconds.

## 3. ★ The gate — PREACT thresholds are walker-tuned (DT-3 / spec A3)

`PREACT_LOWPOWER`'s **wake-on-motion + shake-to-activate thresholds are tuned for the
walker's lift-and-place**, not the rollator's wheel-vibration motion signature (the
frame-mount IMU sees continuous low-amplitude vibration, no lift impulses — coord §C52.2).
Consequences to quantify on the bench **before any rollator ships**:

- **Shake-to-activate** may not fire reliably (or may fire on transit vibration).
- **Wake-on-motion** session auto-start may mis-trigger (rolling vs. lifting) or miss.
- **Transit back-off** must actually damp a vibrating-in-a-box cap without draining.

This is the one genuinely-unproven part of the physical loop. `prj_rollator_cloud`
sidesteps it (no gate); `prj_rollator_pilot` depends on it.

## 4. Build + flash

Same as the bench flow ([`new-prod-unit-bringup.md`](../../../gosteady-portal/docs/playbooks/rollator-firmware-flash.md)),
swapping the overlay + build dir:

```bash
west build -b thingy91x/nrf9151/ns -d build_rollator_pilot_<serial> <src> \
  -- -DEXTRA_CONF_FILE=prj_rollator_pilot.conf \
     -DCONFIG_AWS_IOT_CLIENT_ID_STATIC='"<SERIAL>"'
nrfutil device program --firmware build_rollator_pilot_<serial>/merged.hex \
  --core Application --serial-number <jlink-snr> \
  --options chip_erase_mode=ERASE_ALL,verify=VERIFY_READ,reset=RESET_SYSTEM
```

> Reflashing an already-activated unit boots it **activated** (activation.bin survives
> chip-erase) → straight to normal op, **bypassing the pre-activation gate**. To
> exercise §3 you must **wipe → re-claim** (deactivate, then run the shake→activate
> path). Dark posture also means **no uart0 console** — validate via cloud Shadow +
> the green session LED, not logs.

## 5. Bench validation checklist (the DT-3 exit)

- [ ] **Dark at rest**: no LEDs/console idle; hourly (safety-net) heartbeat still lands in the prod Shadow.
- [ ] **Pre-activation**: rolling the (unclaimed) rollator does **not** open a session (no green, no activity row).
- [x] **Shake-to-activate** ✅ (2026-07-12, GS0002000001): a firm shake opened the blue-pulse window (fresh Shadow heartbeat) + a queued activate cmd in-window → green confirm → `active_monitoring`. Walker-tuned threshold fires on a deliberate rollator shake. *Open:* how firm is needed, and whether normal rolling suffices.
- [ ] **Post-activation auto-start**: normal walking auto-starts a session (green) → activity uploads (active_min/distance, no steps).
- [ ] **Transit back-off / battery**: simulated shipping vibration doesn't hold the connection open / drain; on-battery discharge slope acceptable.

Until this passes, ship-candidate units stay on the DT-3 bench, not with users.

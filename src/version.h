/*
 * GoSteady firmware version string (single source of truth).
 *
 * Used by:
 *   - session.c   stamps GS_FIRMWARE_VERSION_STR into the session file header
 *   - cloud.c     publishes it as the heartbeat / activity `firmware_version`
 *                 / `firmware` field
 *
 * Bump this when shipping a milestone that changes runtime behavior in a
 * way the cloud / portal team should be able to attribute later. Major-
 * version bumps imply a session-file format or telemetry-schema change.
 *
 * History:
 *   0.4.0-dev        M4 session logging
 *   0.6.0-algo       M10 on-device V1 distance algo
 *   0.7.0-cloud      M12.1c.1 cloud bring-up
 *   0.8.0-prod       M10.7 production-telemetry stack + M12.1c.2 hourly
 *                    heartbeat with locked optional extras
 *   0.9.0-hardening  M14.5 hardening sprint per HARDENING_FMEA.md punch-
 *                    list: empty-UTC retro-stamp (1.1+1.2), activity
 *                    PUBACK retry (1.3), forensics counter reset on
 *                    activation (4.1), boot_count in heartbeat (4.7),
 *                    boot-time orphan sweep (6.2), snippet rotation
 *                    policy (6.1).
 *   0.10.0-at-timeout firmware-coord §C11.5: bounded-timeout AT-cmd wrapper
 *                    in cellular.c (at_cmd_with_timeout). Eliminates the
 *                    session_start AT-serialization lockup that produced
 *                    the May 11-12 conference watchdog/fatal cascade
 *                    under sustained SIM-PDN-reject contention. CCLK
 *                    calls now return -ETIMEDOUT after 2 s when modem
 *                    is contended; session.c's FMEA 1.1 retro-stamp
 *                    handles the -EAGAIN cleanly.
 *   0.11.0-wipe-cmd  AA-battery-recycle (portal coord §C20): new `wipe`
 *                    downlink cmd handler in cloud.c + new src/wipe.c
 *                    module. cmd fires from cloud-side end-assignment
 *                    (portal `device-api` Lambda). Firmware wipes
 *                    /lfs/activation.bin + /lfs/sessions/*.dat +
 *                    /snippets/* with a battery_pct >= 0.10 floor.
 *                    Acks via Shadow reported.wipe_complete + heartbeat
 *                    last_cmd_id echo (24h cloud-side ack window per
 *                    DL15). Replaces deprecated charger-gated reset
 *                    (DL6 rewrite — original spec assumed rechargeable
 *                    LiPo + on-charger sanitization; hardware shifted
 *                    to replaceable AAs with 6-12 mo life, so the
 *                    charger checkpoint is unavailable). See portal
 *                    `docs/specs/2026-05-17-aa-battery-recycle.md`.
 *   0.12.0-psm       Battery pilot: request LTE-M Power Saving Mode at
 *                    attach (CONFIG_LTE_PSM_REQ=y, RPTAU=3 h, RAT=2 s in
 *                    prj_cloud.conf + prj_field.conf). Prior builds left
 *                    PSM unrequested, so the modem idled in registered
 *                    I-DRX (~mA) 24/7 between hourly heartbeats — the
 *                    dominant overrun against the 1350 mAh / 30-day pilot
 *                    budget. PSM lets the modem deep-sleep (~µA) between
 *                    app-initiated wakes. Downlink (activate/wipe) latency
 *                    unchanged: cmds still land at the next connect via the
 *                    cloud connection-coordinator (coord §C24). First of a
 *                    staged battery-optimization set; snippet/sampler/
 *                    reporter changes land separately so PSM's effect can
 *                    be isolated in the bench model.
 *   0.13.0-pilot     Battery-pilot overlay (prj_pilot.conf on top of
 *                    prj_field.conf): snippets OFF (no 84 KB opportunistic
 *                    uploads) + CONFIG_GOSTEADY_LOW_POWER bundle (fuel-gauge
 *                    cadence 5 s->60 s + nPM1300 current logged on uart0;
 *                    cellular reporter poll 60 s->30 min). FIELD_MODE (from
 *                    prj_field.conf) also silences the idle purple-blink +
 *                    recording LEDs. Idle-sampler gating + gyro-disable are
 *                    deferred (session-capture hot path). Reported only when
 *                    CONFIG_GOSTEADY_LOW_POWER is set — non-pilot builds
 *                    keep reporting 0.12.0-psm so cloud attribution stays
 *                    accurate per build.
 *   0.13.1-pilot     Pilot LED refinement (CONFIG_GOSTEADY_SESSION_LED): the
 *                    green session LED is re-enabled during recording even in
 *                    FIELD_MODE, so the operator gets a "motion detected /
 *                    recording" confirmation, while the device stays dark at
 *                    rest (idle purple blink still off). 0.13.0-pilot had
 *                    silenced ALL LEDs; this restores just the green one.
 *   0.14.0-shipmode  Pre-activation low-power ("shipping") mode +
 *                    gated sampler (docs/specs/preactivation-lowpower-mode.md).
 *                    (1) Gated sampler [UNCONDITIONAL, all builds]: the 100 Hz
 *                    sampler thread blocks on a new session-start signal
 *                    (session.c) instead of spinning when idle — the largest
 *                    idle-power term. (2) CONFIG_GOSTEADY_PREACT_LOWPOWER
 *                    (prj_pilot.conf): in pre-activation, motion → blue
 *                    "pick-me-up" blink + rate-limited connect, and heartbeat
 *                    drops to a 24 h safety net. Cuts storage draw ~340-410 →
 *                    ~48 mAh/month so a cap can ship/sit for months with no
 *                    pull-tab. Gated-sampler SOAK PASSED 2026-05-31 on
 *                    GS0000000001 (16 start/stop cycles incl. rapid-fire,
 *                    0 faults / 0 dropped samples, clean BMI270 resume/suspend,
 *                    algo ran). Shipped to GS0000000001 (the D2C test unit),
 *                    D2C pre-claim staging intact (coord §C40).
 *   0.15.0-wakewindow Pre-activation motion "wake window" (replaces 0.14.0's
 *                    single rate-limited connect). On a shake the device pulses
 *                    BLUE ~1 Hz and STAYS CONNECTED trying to activate for
 *                    PREACT_WAKE_WINDOW_S (300 s, reset on motion, hard-capped
 *                    at WAKE_WINDOW_MAX_S=600 s); a claim made during the window
 *                    activates near-instantly. Success → solid GREEN confirm →
 *                    normal. 5 min motionless → ship sleep. Transit guard:
 *                    after MAX_WINDOWS (3) failed windows motion only blinks
 *                    until the daily safety-net heartbeat. Gives the user a
 *                    clear interactive "shake to set up" window instead of a
 *                    one-shot connect that could miss a just-after claim.
 *   0.15.1-wakewindow UX fix: the blue pulse cadence rode on the motion-reset
 *                    wait, so shaking shortcut the gap and produced erratic
 *                    extra blinks ("2 quick blinks on motion"). Pulse is now a
 *                    fixed 100/900 ms 1 Hz independent of motion (motion checked
 *                    non-blocking). Removed the legacy blue_blink_burst (2-flash
 *                    burst) entirely; a backed-off cap now stays dark on motion.
 *   *.*.x  (battery)  Voltage-based (OCV) state-of-charge [UNCONDITIONAL, all
 *                    builds] — battery_pct now comes from a voltage→OCV lookup
 *                    (battery.c), not the nrf_fuel_gauge coulomb-fused estimate.
 *                    The lib drifts SoC to 0 over a day at our sub-mA idle draw
 *                    (nPM1300 current unreliable there); observed on
 *                    GS9999999998 reading 0% at 4.086 V / ~86% actual after 3.4
 *                    days, never having browned out (coord §C41). Patch-bumped
 *                    across configs: 0.12.1-psm / 0.13.2-pilot / 0.15.2-wakewindow.
 */

#ifndef GOSTEADY_VERSION_H_
#define GOSTEADY_VERSION_H_

/* Single source of truth, but the pilot overlay is a distinct runtime
 * behavior the cloud/portal team must be able to attribute — so the string
 * tracks CONFIG_GOSTEADY_LOW_POWER at compile time. CONFIG_* macros are
 * always visible via Zephyr's globally-injected autoconf.h, so this resolves
 * to a plain compile-time literal usable in static initializers. */
#if defined(CONFIG_GOSTEADY_PREACT_LOWPOWER)
#define GS_FIRMWARE_VERSION_STR "0.15.2-wakewindow"
#elif defined(CONFIG_GOSTEADY_LOW_POWER)
#define GS_FIRMWARE_VERSION_STR "0.13.2-pilot"
#else
#define GS_FIRMWARE_VERSION_STR "0.12.1-psm"
#endif

#endif /* GOSTEADY_VERSION_H_ */

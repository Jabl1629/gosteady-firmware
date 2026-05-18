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
 */

#ifndef GOSTEADY_VERSION_H_
#define GOSTEADY_VERSION_H_

#define GS_FIRMWARE_VERSION_STR "0.11.0-wipe-cmd"

#endif /* GOSTEADY_VERSION_H_ */

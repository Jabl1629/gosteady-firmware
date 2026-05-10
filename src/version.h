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
 */

#ifndef GOSTEADY_VERSION_H_
#define GOSTEADY_VERSION_H_

#define GS_FIRMWARE_VERSION_STR "0.9.0-hardening"

#endif /* GOSTEADY_VERSION_H_ */

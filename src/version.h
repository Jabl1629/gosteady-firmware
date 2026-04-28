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
 *   0.4.0-dev    M4 session logging
 *   0.6.0-algo   M10 on-device V1 distance algo
 *   0.7.0-cloud  M12.1c.1 cloud bring-up
 *   0.8.0-prod   M10.7 production-telemetry stack + M12.1c.2 hourly
 *                heartbeat with locked optional extras
 */

#ifndef GOSTEADY_VERSION_H_
#define GOSTEADY_VERSION_H_

#define GS_FIRMWARE_VERSION_STR "0.8.0-prod"

#endif /* GOSTEADY_VERSION_H_ */

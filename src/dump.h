/*
 * Session dump channel (Milestone 5).
 *
 * Binds a Zephyr UART device to a tiny line-based command protocol so
 * the host can list, pull, and delete session files from /lfs/sessions.
 *
 * Wire protocol (newline-terminated ASCII on the command side; binary
 * body only on DUMP):
 *
 *   > LIST\n
 *   < <uuid> <size_bytes>\n
 *   < <uuid> <size_bytes>\n
 *   < END\n
 *
 *   > DUMP <uuid>\n
 *   < SIZE <size_bytes>\n
 *   < <raw size_bytes bytes>
 *   < \nOK\n              (if all bytes transferred cleanly)
 *   <   or  \nERR ...\n   (on read error mid-stream)
 *
 *   > DEL <uuid>\n
 *   < OK\n                or  ERR <message>\n
 *
 *   Any unknown command -> ERR unknown\n
 *
 * Errors on malformed lines reply with ERR <message>\n. The channel is
 * stateless — each command is independent.
 *
 * On the Thingy:91 X + connectivity-bridge setup, the uart1 side lands
 * on /dev/cu.usbmodem105 at 1 Mbaud on the host.
 */

#ifndef GOSTEADY_DUMP_H_
#define GOSTEADY_DUMP_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Start the dump-channel thread. Returns 0 on success, negative errno
 * if the UART device isn't ready. */
int gosteady_dump_start(void);

#ifdef __cplusplus
}
#endif

#endif

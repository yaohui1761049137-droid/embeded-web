/* remote.h — Remote Board protocol client
 *
 * Owns everything above the byte layer for talking to Remote Boards
 * over serial: the board table (port key → device/baud), the PING
 * warmup, the retry policy, and the OK/ERR response grammar.
 * network.cgi.c only looks up a board and calls remote_query /
 * remote_configure; it no longer knows the protocol.
 *
 * Wire protocol is unchanged (README "串口协议" table); handler.sh
 * is the Remote Board side mirror.
 *
 * Offline testing: the device path resolves through
 * REMOTE_SERIAL_DEVICE_OVERRIDE when set (like gate's DB_PATH).
 */
#ifndef REMOTE_H
#define REMOTE_H

#include <stddef.h>

typedef struct RemoteBoard RemoteBoard;

typedef struct {
    char ip[64];
    char mask[64];
    char gateway[64];
    char ipv6[128];
} RemoteNetConfig;

/* Map a query-string port key to a board index: NULL/"" → Board 1
 * (default), "s4" → Board 2, anything else → -1.  Board 3/4 = add
 * a row to the table in remote.c. */
int remote_board_lookup(const char *port);

/* Open the board's serial device and do the PING warmup (result
 * discarded — original wire behavior: one PING per request).
 * Returns NULL on open failure. */
RemoteBoard *remote_board_open(int board);

/* Query IPv4 + IPv6: runs REMOTE_GET_IPV4 then REMOTE_GET_IPV6.
 * Fields of failed commands stay empty.  Returns 0/-1 with err set;
 * the caller may ignore the rc to preserve the original "ok with
 * empty fields" wire behavior. */
int remote_query(RemoteBoard *h, RemoteNetConfig *out, char *err, size_t errlen);

/* Configure IPv4 + optional IPv6 (only when cfg->ipv6 non-empty).
 * err is composed as "IPv4: <msg>" / "IPv4: <msg>; IPv6: <msg>"
 * (matching the original combined error message; the original
 * produced an empty message when only IPv6 failed). */
int remote_configure(RemoteBoard *h, const RemoteNetConfig *cfg,
                     char *err, size_t errlen);

void remote_board_close(RemoteBoard *h);

#endif /* REMOTE_H */

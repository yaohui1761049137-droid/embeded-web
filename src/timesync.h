/* timesync.h — PPS+TOD+chrony sync status and UT986 receiver mode control
 *
 * Status: chronyc -c sources/tracking plus the pps_tod daemon's
 * per-second state files (/run/pps_tod/status, watchdog.state).
 * Receiver-mode control sends fixed $CFGGNSS/$CFGSAVE payloads to the
 * TOD serial device (ttyS7).
 *
 * Privileges: chronyc is a plain local client (no sudo); the serial
 * device is reachable because www-data is in the dialout group on the
 * board.  Host tests override TIMESYNC_DEV (serial device),
 * TIMESYNC_STATUS_DIR (state file directory) and CHRONYC_OVERRIDE
 * (chronyc executable) — same pattern as NMCLI_OVERRIDE.
 */
#ifndef TIMESYNC_H
#define TIMESYNC_H

#include <stddef.h>

/* Receiver mode whitelist — UT986 $CFGGNSS sysMask values (protocol
 * spec 1.4.2.7; payload checksums verified against the XOR rule). */
typedef enum {
    TS_MODE_UNKNOWN = 0,
    TS_MODE_GNSS,   /* GNSS 全系统 (factory default) */
    TS_MODE_GPS,
    TS_MODE_BDS,    /* 北斗 */
    TS_MODE_GAL,    /* Galileo */
    TS_MODE_GLO     /* GLONASS */
} TimesyncMode;

/* Whitelist lookup; returns TS_MODE_UNKNOWN outside the five modes. */
TimesyncMode timesync_mode_parse(const char *name);

/* Human label ("gnss" → "GNSS 全系统"); NULL if unknown. */
const char *timesync_mode_label(TimesyncMode m);

/* Send $CFGGNSS + $CFGSAVE to the receiver.  Write-only, fixed byte
 * payloads; never reads the device (pps_tod owns the RX path) and
 * never touches termios (device-shared state).  Returns 0 on success,
 * -1 with err set. */
int timesync_set_mode(TimesyncMode m, char *err, size_t errlen);

/* Last-applied mode bookkeeping (shown in the UI).  The command channel
 * is write-only — the receiver cannot be re-read without stealing NMEA
 * sentences from pps_tod — so the UI displays what was last set here,
 * not a live readback.  State file: TIMESYNC_MODE_FILE override,
 * default /var/db/ut986_mode (key=value: mode, ts). */
int timesync_mode_save(TimesyncMode m, char *err, size_t errlen);

/* Read the last-set mode back.  Returns 1 if present (mode name into
 * out, timestamp into ts), 0 if no state file / no mode line, -1 on
 * error.  out/ts may be NULL. */
int timesync_mode_last(char *out, size_t outlen, char *ts, size_t tslen);

/* Build the status JSON document into out.  Sections degrade
 * individually (chrony down, state file missing) but the document
 * always has a well-formed shape for the UI.  Returns 0 on success,
 * -1 if out was too small. */
int timesync_status_json(char *out, size_t outlen, char *err, size_t errlen);

#endif /* TIMESYNC_H */
/* common.h — CGI helpers: HTTP output, serial port, POST parsing
 *
 * Phase 2: File-based session functions removed (moved to auth.c).
 * Cookie name changed from "boa_token" to "session_id".
 */
#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

/* ── Cookie name (Phase 2: renamed from boa_token) ───────────────── */
#define SESSION_COOKIE_NAME "session_id"

/* ── HTTP helpers ─────────────────────────────────────────────────── */
void cgi_header(const char *content_type);
void cgi_redirect(const char *url);
char *get_env(const char *key);

/* Parse Cookie header to get a specific cookie value.
 * Returns pointer to static buffer, or NULL if not found. */
char *get_cookie(const char *name);

/* ── Serial port ──────────────────────────────────────────────────── */
#define SERIAL_DEVICE       "/dev/ttyFIQ0"
#define CUSTOM_BAUD         1500000

/* Remote Board devices/bauds live in the remote module's board table
 * (src/remote.c) — the CGI used to pick them via port=s4 if/else. */

int  serial_open(const char *device, unsigned int baudrate);
int  serial_send(int fd, const char *data, int len);
void serial_close(int fd);

/* Read one line (until \n) from serial fd with timeout_ms.
 * Returns bytes read (without \n/\r), 0 on timeout, -1 on error.
 * Strips \r; result is NUL-terminated. */
int  serial_read_line(int fd, char *buf, int max_len, int timeout_ms);

/* ── POST body parsing ────────────────────────────────────────────── */
/* Extract a parameter value from POST body (x-www-form-urlencoded).
 * Returns a freshly allocated URL-decoded copy, or NULL if the param
 * is absent. The copy is never freed (CGI process exits at request end). */
char *get_post_param(const char *param_name);

#endif /* COMMON_H */

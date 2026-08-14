/* gate.h — CGI request gate: session → role → CSRF ladder
 *
 * Every protected CGI starts with one gate call instead of hand-rolling the
 * cookie → auth_init → auth_session_verify → error/redirect ladder.
 *
 * Contract:
 *  - gate_*_session() on FAILURE emits the error (JSON) or redirect (page
 *    mode) itself, cleans up the DB, and returns 0 — the caller must not
 *    touch auth afterwards.
 *  - On SUCCESS the DB (g_db) is left open; the caller still calls
 *    auth_cleanup() when done (main.cgi closes early to save resources).
 *  - No cookie → no DB open (cheap rejection for unauthenticated requests).
 *  - DB path: $DB_PATH env override, default /var/db/myapp.db.
 */

#ifndef GATE_H
#define GATE_H

#include "auth.h"

#define DB_PATH_DEFAULT "/var/db/myapp.db"

/* Resolve DB path: $DB_PATH or default (for auth_init / direct sqlite open) */
const char *gate_db_path(void);

/* JSON mode: emits {"status":"error",...} on failure. Returns 1 ok, 0 failed. */
int gate_json_session(SessionInfo *out);

/* Page mode: emits 302 → /index.html on failure (302 → /change.html when
 * the session user must change password, ADR-0002). Returns 1 ok, 0 failed. */
int gate_page_session(SessionInfo *out);

/* JSON session gate that skips the forced-change check — only the
 * change-password CGI may use this (ADR-0002). */
int gate_json_session_no_policy(SessionInfo *out);

/* Role ladder step: emits Forbidden JSON if session lacks the role. */
int gate_require_role(const SessionInfo *s, const char *role);

/* CSRF ladder step: emits error JSON if token mismatch. */
int gate_require_csrf(const SessionInfo *s, const char *token);

/* Emit Set-Cookie headers (login sets them; logout passes NULL, NULL to clear).
 * Caller prints "Status: 302" and "Location:" around this. */
void gate_emit_session_cookies(const char *sid, const char *csrf);

#endif /* GATE_H */

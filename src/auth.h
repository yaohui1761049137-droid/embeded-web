/* auth.h — Multi-user authentication library for LubanCat
 *
 * Replaces file-based session management in common.c with SQLite-backed
 * session/user/CSRF support.
 *
 * Dependencies: sqlite3.c (amalgamation), sha256.c (self-contained)
 * Compile with: gcc -Wall -O2 -o prog auth.c sqlite3.c sha256.c common.c
 *
 * Password format: $5$rounds=N$salt$hash  (SHA-256 crypt compatible)
 */

#ifndef AUTH_H
#define AUTH_H

#include <stdint.h>
#include <time.h>
#include "sqlite3.h"

/* ── Token lengths ──────────────────────────────────────────────── */
#define SESSION_ID_LEN     64    /* hex encoded, 32 bytes from urandom */
#define CSRF_TOKEN_LEN     32    /* hex encoded, 16 bytes from urandom */
#define SESSION_EXPIRE     3600  /* 1 hour, in seconds */
#define PASSWORD_SALT_LEN  16    /* bytes for salt */
#define PASSWORD_ROUNDS    100000 /* SHA-256 iterations */

/* ── Password policy (ADR-0002) ──────────────────────────────────── */
#define PASSWORD_MIN_LEN     10    /* strong policy minimum */
#define PASSWORD_MAX_LEN     64    /* existing hard cap */
#define PASSWORD_EXPIRE_DAYS 90    /* validity after each change */
#define PASSWORD_WARN_DAYS    7    /* warn on login when ≤7 days left */

/* ── Session info ────────────────────────────────────────────────── */
typedef struct {
    int    user_id;
    char   username[64];
    char   role[16];             /* "root" or "admin" */
    char   csrf_token[CSRF_TOKEN_LEN + 1];
} SessionInfo;

/* ── Database ────────────────────────────────────────────────────── */
int  auth_init(const char *db_path);
void auth_cleanup(void);

/* Return the open database handle (NULL if auth_init not called).
 * Lets sibling modules (users.c) operate on the same connection
 * instead of opening a second one. */
sqlite3 *auth_db(void);

/* ── Session ─────────────────────────────────────────────────────── */
int  auth_session_create(int user_id, const char *client_ip,
                         char *sid_out, char *csrf_out);
int  auth_session_verify(const char *sid, SessionInfo *out);
void auth_session_destroy(const char *sid);
void auth_session_cleanup(void);

/* ── Password ────────────────────────────────────────────────────── */
/* Hash a password.  hash_out must be >= 128 bytes.
 * Returns 0 on success, -1 on error. */
int  auth_hash_password(const char *password, char *hash_out, int max_hash);

/* Verify password against stored hash.  Returns 1 if match, 0 if not. */
int  auth_verify_password(const char *password, const char *stored_hash);

/* ── User management ─────────────────────────────────────────────── */
/* Verify login credentials.  Returns user_id on success, -1 on failure. */
int  auth_user_login(const char *username, const char *password,
                     int *user_id_out);

/* ── Password policy (ADR-0002) ──────────────────────────────────── */
/* Validate password against strong policy (10-64 chars, one each of
 * upper/lower/digit/ASCII-punct, no non-ASCII/space).  err gets a
 * Chinese reason on failure (may be NULL).  Returns 1 if OK, 0 if not. */
int  auth_password_policy_ok(const char *password, char *err, int err_max);

/* 1 if user must change password on next login (never set, i.e. legacy
 * account, or older than PASSWORD_EXPIRE_DAYS), 0 otherwise. */
int  auth_user_must_change_password(int user_id);

/* Days left until password expiry (0 if expired/never set), -1 if no user. */
int  auth_user_days_left(int user_id);

/* Delete all sessions of user_id except keep_sid (NULL = delete all). */
void auth_kick_user_sessions(int user_id, const char *keep_sid);

/* ── Permissions ─────────────────────────────────────────────────── */
/* Check if session has a given role.  root has all permissions.
 * Returns 1 if authorized, 0 if not. */
int  auth_require_role(const SessionInfo *s, const char *role);

/* ── CSRF ────────────────────────────────────────────────────────── */
int  auth_csrf_verify(const SessionInfo *s, const char *token);

/* ── Audit ───────────────────────────────────────────────────────── */
void auth_audit_log(int operator_id, const char *action,
                    int target_user_id, const char *detail,
                    const char *client_ip);

/* ── Token generation ────────────────────────────────────────────── */
/* Generate a cryptographically random hex token.
 * Reads /dev/urandom.  out must be len+1 bytes. */
void auth_generate_token(char *out, int len);

#endif /* AUTH_H */

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

/* ── Token lengths ──────────────────────────────────────────────── */
#define SESSION_ID_LEN     64    /* hex encoded, 32 bytes from urandom */
#define CSRF_TOKEN_LEN     32    /* hex encoded, 16 bytes from urandom */
#define SESSION_EXPIRE     3600  /* 1 hour, in seconds */
#define PASSWORD_SALT_LEN  16    /* bytes for salt */
#define PASSWORD_ROUNDS    100000 /* SHA-256 iterations */

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

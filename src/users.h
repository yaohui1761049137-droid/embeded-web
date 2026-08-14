/* users.h — User management module: domain rules on top of auth.c
 *
 * Concentrates the user-management business invariants that used to
 * live scattered across the five user_*.cgi files:
 *   - cannot delete self / cannot delete root
 *   - cannot disable self / cannot disable the last enabled root
 *   - username/password validation (3-32 / 6-64 chars)
 * Audit rows are written inside each mutating operation with the real
 * target_user_id (the old CGIs passed 0 for create, which silently
 * failed the audit_log FK constraint).
 *
 * DB handle comes from auth_db() — callers must have run auth_init()
 * first (the request gate already does).  CGI layer sits on top:
 * parse params → call users_* → render JSON.
 */
#ifndef USERS_H
#define USERS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int     id;
    char    username[64];
    char    role[16];             /* "root" or "admin" */
    int     enabled;
    int64_t created_at;
    int64_t last_login_at;
} UserRow;

/* Mutating ops: return 0 on success, -1 on failure with a human
 * message in err (if err/errlen given).  Audit rows are written on
 * success with the correct target_user_id. */
int users_create(int actor_id, const char *username, const char *password,
                 char *err, size_t errlen);
int users_delete(int actor_id, int target_id, char *err, size_t errlen);
int users_toggle(int actor_id, int target_id, int enabled,
                 char *err, size_t errlen);
int users_passwd(int actor_id, int target_id, const char *password,
                 char *err, size_t errlen);

/* Fetch all users ordered by id.  Returns 0 on success; *out is a
 * caller-allocated array that must be freed with free(). */
int users_fetch_all(UserRow **out, int *count);

#endif /* USERS_H */

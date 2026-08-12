/* user_change_pass.cgi — ADR-0002: self-service password change
 *
 * Any authenticated user changes their own password:
 *   - requires current password (anti session-hijack)
 *   - new password must satisfy the strong policy and differ from old
 *   - restarts the 90-day validity timer
 *   - kicks the user's other sessions (keeps the current one)
 *
 * NOT subject to the forced-change gate (must stay reachable), but
 * still requires a valid session + CSRF.
 */
#include "common.h"
#include "auth.h"
#include "sqlite3.h"
#include <time.h>

#define DB_PATH "/var/db/myapp.db"

int main(void) {
    auth_init(DB_PATH);

    const char *sid = get_cookie(SESSION_COOKIE_NAME);
    SessionInfo session;
    if (!sid || !auth_session_verify(sid, &session)) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Not authenticated\"}");
        auth_cleanup();
        return 0;
    }

    const char *csrf = get_post_param("csrf_token");
    if (!csrf || !auth_csrf_verify(&session, csrf)) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"CSRF token invalid\"}");
        auth_cleanup();
        return 0;
    }

    const char *old_pass = get_post_param("old_password");
    const char *new_pass = get_post_param("new_password");

    if (!old_pass || !*old_pass || !new_pass || !*new_pass) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Missing parameters\"}");
        auth_cleanup();
        return 0;
    }

    /* Verify current password */
    if (auth_user_login(session.username, old_pass, NULL) < 0) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"当前密码不正确\"}");
        auth_cleanup();
        return 0;
    }

    /* ADR-0002: strong password policy */
    char policy_err[128];
    if (!auth_password_policy_ok(new_pass, policy_err, sizeof(policy_err))) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"%s\"}", policy_err);
        auth_cleanup();
        return 0;
    }

    sqlite3 *mydb;
    sqlite3_open(DB_PATH, &mydb);
    sqlite3_stmt *stmt;
    time_t now = time(NULL);
    int ok = 0;

    /* No reuse: new password must differ from the stored one (hash
     * comparison — the stored format is salted+iterated, not plaintext) */
    if (sqlite3_prepare_v2(mydb,
            "SELECT password_hash FROM users WHERE id = ?",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, session.user_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *old_hash =
                (const char *)sqlite3_column_text(stmt, 0);
            if (auth_verify_password(new_pass, old_hash)) {
                cgi_header("application/json");
                printf("{\"status\":\"error\",\"message\":\"新密码不能与当前密码相同\"}");
                sqlite3_finalize(stmt);
                sqlite3_close(mydb);
                auth_cleanup();
                return 0;
            }
        }
        sqlite3_finalize(stmt);
    }

    char hash[256];
    if (auth_hash_password(new_pass, hash, sizeof(hash)) != 0) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Password hashing failed\"}");
        sqlite3_close(mydb);
        auth_cleanup();
        return 0;
    }

    /* Update hash + restart timer */
    const char *sql =
        "UPDATE users SET password_hash = ?, password_changed_at = ?, "
        "updated_at = ? WHERE id = ?";

    if (sqlite3_prepare_v2(mydb, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text (stmt, 1, hash, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, (int64_t)now);
        sqlite3_bind_int64(stmt, 3, (int64_t)now);
        sqlite3_bind_int  (stmt, 4, session.user_id);
        ok = (sqlite3_step(stmt) == SQLITE_DONE)
          && (sqlite3_changes(mydb) > 0);
        sqlite3_finalize(stmt);
    }

    /* Keep the current session, kick all others */
    if (ok) auth_kick_user_sessions(session.user_id, sid);

    sqlite3_close(mydb);

    cgi_header("application/json");
    if (ok) {
        auth_audit_log(session.user_id, "change_password", session.user_id,
                       "", getenv("REMOTE_ADDR"));
        printf("{\"status\":\"ok\",\"message\":\"密码修改成功\"}");
    } else {
        printf("{\"status\":\"error\",\"message\":\"Password update failed\"}");
    }

    auth_cleanup();
    return 0;
}

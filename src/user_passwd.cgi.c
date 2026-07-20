/* user_passwd.cgi — Phase 2: reset user password (root only, POST) */
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

    if (!auth_require_role(&session, "root")) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Forbidden\"}");
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

    const char *target_id_str = get_post_param("user_id");
    const char *new_pass = get_post_param("password");

    if (!target_id_str || !new_pass ||
        strlen(new_pass) < 6 || strlen(new_pass) > 64) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Invalid parameters\"}");
        auth_cleanup();
        return 0;
    }

    int target_id = atoi(target_id_str);
    char hash[256];
    if (auth_hash_password(new_pass, hash, sizeof(hash)) != 0) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Password hashing failed\"}");
        auth_cleanup();
        return 0;
    }

    sqlite3 *mydb;
    sqlite3_open(DB_PATH, &mydb);
    sqlite3_stmt *stmt;
    time_t now = time(NULL);

    const char *sql =
        "UPDATE users SET password_hash = ?, updated_at = ? WHERE id = ?";

    int ok = 0;
    if (sqlite3_prepare_v2(mydb, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text (stmt, 1, hash, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, (int64_t)now);
        sqlite3_bind_int  (stmt, 3, target_id);
        ok = (sqlite3_step(stmt) == SQLITE_DONE)
          && (sqlite3_changes(mydb) > 0);
        sqlite3_finalize(stmt);
    }

    sqlite3_close(mydb);

    cgi_header("application/json");
    if (ok) {
        auth_audit_log(session.user_id, "reset_password", target_id,
                       "", getenv("REMOTE_ADDR"));
        printf("{\"status\":\"ok\",\"message\":\"Password updated\"}");
    } else {
        printf("{\"status\":\"error\",\"message\":\"User not found\"}");
    }

    auth_cleanup();
    return 0;
}

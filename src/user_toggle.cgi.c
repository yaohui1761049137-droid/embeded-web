/* user_toggle.cgi — Phase 2: enable/disable user (root only, POST) */
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

    const char *id_str = get_post_param("user_id");
    const char *en_str = get_post_param("enabled");

    if (!id_str || !en_str) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Missing parameters\"}");
        auth_cleanup();
        return 0;
    }

    int user_id = atoi(id_str);
    int enabled = atoi(en_str) ? 1 : 0;

    /* Protection: cannot disable yourself */
    if (user_id == session.user_id) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Cannot disable yourself\"}");
        auth_cleanup();
        return 0;
    }

    /* Protection: cannot disable last root */
    if (!enabled) {
        sqlite3 *mydb;
        sqlite3_open(DB_PATH, &mydb);
        sqlite3_stmt *stmt;
        const char *role_sql = "SELECT role FROM users WHERE id = ?";
        int is_root = 0;
        if (sqlite3_prepare_v2(mydb, role_sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, user_id);
            if (sqlite3_step(stmt) == SQLITE_ROW)
                is_root = (strcmp((const char *)sqlite3_column_text(stmt, 0), "root") == 0);
            sqlite3_finalize(stmt);
        }
        if (is_root) {
            const char *cnt_sql = "SELECT COUNT(*) FROM users WHERE role='root' AND enabled=1";
            if (sqlite3_prepare_v2(mydb, cnt_sql, -1, &stmt, NULL) == SQLITE_OK) {
                if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) <= 1) {
                    cgi_header("application/json");
                    printf("{\"status\":\"error\",\"message\":\"Cannot disable last root user\"}");
                    sqlite3_finalize(stmt);
                    sqlite3_close(mydb);
                    auth_cleanup();
                    return 0;
                }
                sqlite3_finalize(stmt);
            }
        }
        sqlite3_close(mydb);
    }

    /* Update */
    sqlite3 *mydb;
    sqlite3_open(DB_PATH, &mydb);
    sqlite3_stmt *stmt;
    time_t now = time(NULL);

    const char *sql = "UPDATE users SET enabled = ?, updated_at = ? WHERE id = ?";
    int ok = 0;
    if (sqlite3_prepare_v2(mydb, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int  (stmt, 1, enabled);
        sqlite3_bind_int64(stmt, 2, (int64_t)now);
        sqlite3_bind_int  (stmt, 3, user_id);
        ok = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(mydb) > 0);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(mydb);

    cgi_header("application/json");
    if (ok) {
        auth_audit_log(session.user_id, enabled ? "enable_user" : "disable_user",
                       user_id, "", getenv("REMOTE_ADDR"));
        printf("{\"status\":\"ok\",\"message\":\"User %s\"}",
               enabled ? "enabled" : "disabled");
    } else {
        printf("{\"status\":\"error\",\"message\":\"User not found\"}");
    }

    auth_cleanup();
    return 0;
}

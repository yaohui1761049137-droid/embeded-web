/* user_delete.cgi — Phase 2: delete user (root only, POST) */
#include "common.h"
#include "auth.h"
#include "sqlite3.h"

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
    if (!id_str) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Missing user_id\"}");
        auth_cleanup();
        return 0;
    }

    int target_id = atoi(id_str);

    /* Protection 1: cannot delete yourself */
    if (target_id == session.user_id) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Cannot delete yourself\"}");
        auth_cleanup();
        return 0;
    }

    sqlite3 *mydb;
    sqlite3_open(DB_PATH, &mydb);

    /* Protection 2: cannot delete root */
    sqlite3_stmt *stmt;
    const char *role_sql = "SELECT role, username FROM users WHERE id = ?";
    char target_role[16] = "";
    char target_name[64] = "";
    int is_root = 0;

    if (sqlite3_prepare_v2(mydb, role_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, target_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            strncpy(target_role, (const char *)sqlite3_column_text(stmt, 0), 15);
            strncpy(target_name, (const char *)sqlite3_column_text(stmt, 1), 63);
            is_root = (strcmp(target_role, "root") == 0);
        }
        sqlite3_finalize(stmt);
    }

    if (is_root) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Cannot delete root user\"}");
        sqlite3_close(mydb);
        auth_cleanup();
        return 0;
    }

    /* Protection 3: cannot delete last root (edge case safeguard) */
    const char *cnt_sql = "SELECT COUNT(*) FROM users WHERE role='root' AND enabled=1";
    if (sqlite3_prepare_v2(mydb, cnt_sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) < 1) {
            cgi_header("application/json");
            printf("{\"status\":\"error\",\"message\":\"No enabled root would remain\"}");
            sqlite3_finalize(stmt);
            sqlite3_close(mydb);
            auth_cleanup();
            return 0;
        }
        sqlite3_finalize(stmt);
    }

    /* Delete */
    int ok = 0;
    const char *del_sql = "DELETE FROM users WHERE id = ? AND role != 'root'";
    if (sqlite3_prepare_v2(mydb, del_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, target_id);
        ok = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(mydb) > 0);
        sqlite3_finalize(stmt);
    }

    sqlite3_close(mydb);

    cgi_header("application/json");
    if (ok) {
        auth_audit_log(session.user_id, "delete_user", target_id,
                       target_name, getenv("REMOTE_ADDR"));
        printf("{\"status\":\"ok\",\"message\":\"User deleted\"}");
    } else {
        printf("{\"status\":\"error\",\"message\":\"User not found or cannot be deleted\"}");
    }

    auth_cleanup();
    return 0;
}

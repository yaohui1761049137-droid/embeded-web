/* user_list.cgi — Phase 2: list all users (root only)
 * GET: returns JSON array of users (excludes password_hash).
 */
#include "common.h"
#include "auth.h"
#include "sqlite3.h"

#define DB_PATH "/var/db/myapp.db"

int main(void) {
    auth_init(DB_PATH);

    /* Auth */
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

    /* Query users */
    sqlite3 *mydb;
    sqlite3_open(DB_PATH, &mydb);
    sqlite3_stmt *stmt;

    const char *sql =
        "SELECT id, username, role, enabled, created_at, last_login_at "
        "FROM users ORDER BY id";

    cgi_header("application/json; charset=utf-8");
    printf("{\"status\":\"ok\",\"users\":[");

    if (sqlite3_prepare_v2(mydb, sql, -1, &stmt, NULL) == SQLITE_OK) {
        int first = 1;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) printf(",");
            printf("{\"id\":%d,\"username\":\"%s\",\"role\":\"%s\","
                   "\"enabled\":%d,\"created_at\":%lld,\"last_login_at\":%lld}",
                   sqlite3_column_int(stmt, 0),
                   sqlite3_column_text(stmt, 1),
                   sqlite3_column_text(stmt, 2),
                   sqlite3_column_int(stmt, 3),
                   (long long)sqlite3_column_int64(stmt, 4),
                   (long long)sqlite3_column_int64(stmt, 5));
            first = 0;
        }
        sqlite3_finalize(stmt);
    }

    printf("]}");
    sqlite3_close(mydb);
    auth_cleanup();
    return 0;
}

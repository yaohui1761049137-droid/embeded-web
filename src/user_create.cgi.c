/* user_create.cgi — Phase 2: create admin user (root only, POST) */
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

    /* CSRF */
    const char *csrf = get_post_param("csrf_token");
    if (!csrf || !auth_csrf_verify(&session, csrf)) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"CSRF token invalid\"}");
        auth_cleanup();
        return 0;
    }

    const char *new_user = get_post_param("username");
    const char *new_pass = get_post_param("password");

    if (!new_user || strlen(new_user) < 3 || strlen(new_user) > 32 ||
        !new_pass || strlen(new_pass) < 6 || strlen(new_pass) > 64) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Invalid parameters "
               "(username: 3-32 chars, password: 6-64 chars)\"}");
        auth_cleanup();
        return 0;
    }

    /* Hash password */
    char hash[256];
    if (auth_hash_password(new_pass, hash, sizeof(hash)) != 0) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Password hashing failed\"}");
        auth_cleanup();
        return 0;
    }

    /* Insert user */
    sqlite3 *mydb;
    sqlite3_open(DB_PATH, &mydb);
    sqlite3_stmt *stmt;
    time_t now = time(NULL);

    const char *sql =
        "INSERT INTO users (username, password_hash, role, enabled, "
        "created_at, updated_at, created_by) VALUES (?, ?, 'admin', 1, ?, ?, ?)";

    int ok = 0;
    if (sqlite3_prepare_v2(mydb, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text (stmt, 1, new_user, -1, SQLITE_STATIC);
        sqlite3_bind_text (stmt, 2, hash,     -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 3, (int64_t)now);
        sqlite3_bind_int64(stmt, 4, (int64_t)now);
        sqlite3_bind_int  (stmt, 5, session.user_id);
        ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }

    sqlite3_close(mydb);

    cgi_header("application/json");
    if (ok) {
        /* Audit */
        auth_audit_log(session.user_id, "create_user", 0, new_user,
                       getenv("REMOTE_ADDR"));
        printf("{\"status\":\"ok\",\"message\":\"User created\"}");
    } else {
        printf("{\"status\":\"error\",\"message\":\"Username may already exist\"}");
    }

    auth_cleanup();
    return 0;
}

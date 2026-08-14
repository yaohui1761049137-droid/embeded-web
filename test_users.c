/* test_users.c — host-side unit tests for the users module (offline)
 *
 * Tests the module's business invariants directly at the seam:
 *   - self-delete / root-delete refused
 *   - last-enabled-root disable refused
 *   - audit rows written with the correct target_user_id
 *     (regression: user_create.cgi used to pass 0 → FK violation,
 *      silently dropping the audit row)
 *   - username/password validation, duplicate detection
 *   - login blocked when a user is disabled
 *
 * Compile (host): gcc -Wall -O2 -o test_users test_users.c \
 *                 src/users.c src/auth.c src/sha256.c \
 *                 src/sqlite3.c -lpthread -ldl
 * Usage: ./test_users [db_path]   (default ./test_users.db, removed first)
 */
#include "src/users.h"
#include "src/auth.h"
#include "src/sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_pass = 0, g_fail = 0;

static int create_v1_db(const char *path);
static const char *fk_action(const char *table);
static int table_count(const char *table);

static void check(int cond, const char *desc) {
    if (cond) { g_pass++; printf("  ✅ %s\n", desc); }
    else      { g_fail++; printf("  ❌ %s\n", desc); }
}

static void expect_fail(int rc, const char *err, const char *needle,
                        const char *desc) {
    int ok = (rc != 0 && err && strstr(err, needle) != NULL);
    check(ok, desc);
    if (!ok) printf("      (rc=%d, err=\"%s\")\n", rc, err ? err : "(null)");
}

static int seed_user(const char *username, const char *role, int enabled) {
    char hash[256];
    if (auth_hash_password("testpass123", hash, sizeof(hash)) != 0) return -1;
    sqlite3 *db = auth_db();
    sqlite3_stmt *stmt;
    const char *sql =
        "INSERT INTO users (username, password_hash, role, enabled, "
        "created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    time_t now = time(NULL);
    sqlite3_bind_text (stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text (stmt, 2, hash,     -1, SQLITE_STATIC);
    sqlite3_bind_text (stmt, 3, role,     -1, SQLITE_STATIC);
    sqlite3_bind_int  (stmt, 4, enabled);
    sqlite3_bind_int64(stmt, 5, (int64_t)now);
    sqlite3_bind_int64(stmt, 6, (int64_t)now);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return (int)sqlite3_last_insert_rowid(db);
}

static int find_id(const char *username) {
    sqlite3 *db = auth_db();
    sqlite3_stmt *stmt;
    int id = -1;
    const char *sql = "SELECT id FROM users WHERE username = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            id = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return id;
}

static int audit_count(const char *action, int target_id) {
    sqlite3 *db = auth_db();
    sqlite3_stmt *stmt;
    int n = 0;
    const char *sql =
        "SELECT COUNT(*) FROM audit_log WHERE action = ? AND target_user_id = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, action, -1, SQLITE_STATIC);
        sqlite3_bind_int (stmt, 2, target_id);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            n = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return n;
}

static int audit_null_count(const char *action) {
    sqlite3 *db = auth_db();
    sqlite3_stmt *stmt;
    int n = 0;
    const char *sql =
        "SELECT COUNT(*) FROM audit_log WHERE action = ? AND target_user_id IS NULL";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, action, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            n = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return n;
}

int main(int argc, char **argv) {
    const char *db_path = (argc > 1) ? argv[1] : "test_users.db";
    remove(db_path);
    if (auth_init(db_path) != 0) {
        fprintf(stderr, "auth_init failed\n");
        return 1;
    }

    char err[128];
    int root_id = seed_user("root", "root", 1);
    check(root_id > 0, "seed root");

    /* ── fetch_all ─────────────────────────────────────────────── */
    UserRow *rows = NULL;
    int n = 0;
    check(users_fetch_all(&rows, &n) == 0 && n == 1 &&
          strcmp(rows[0].username, "root") == 0 &&
          strcmp(rows[0].role, "root") == 0,
          "fetch_all: one root user");
    free(rows);

    /* ── create + audit (FK regression) ────────────────────────── */
    check(users_create(root_id, "alice", "pass123456",
                       err, sizeof(err)) == 0, "create alice ok");
    int alice_id = find_id("alice");
    check(alice_id > 0, "alice in DB");
    check(audit_count("create_user", alice_id) == 1,
          "audit: create_user targets alice (old code wrote target=0, "
          "FK silently dropped the row)");
    check(users_create(root_id, "bob", "pass123456",
                       err, sizeof(err)) == 0, "create bob ok");
    int bob_id = find_id("bob");
    check(bob_id > 0, "bob in DB");

    /* ── validation ────────────────────────────────────────────── */
    expect_fail(users_create(root_id, "alice", "pass123456",
                             err, sizeof(err)), err, "already exist",
                "duplicate username refused");
    expect_fail(users_create(root_id, "ab", "pass123456",
                             err, sizeof(err)), err, "Invalid parameters",
                "short username refused");
    expect_fail(users_create(root_id, "carol", "pass1",
                             err, sizeof(err)), err, "Invalid parameters",
                "short password refused");
    expect_fail(users_create(root_id, NULL, NULL,
                             err, sizeof(err)), err, "Invalid parameters",
                "null params refused");

    /* ── delete invariants ─────────────────────────────────────── */
    expect_fail(users_delete(root_id, root_id, err, sizeof(err)),
                err, "Cannot delete yourself", "self-delete refused");
    expect_fail(users_delete(root_id, 999, err, sizeof(err)),
                err, "User not found", "delete nonexistent refused");
    check(users_delete(root_id, alice_id, err, sizeof(err)) == 0,
          "delete alice ok (FK would block: audit row + no session)");
    check(audit_null_count("delete_user") == 1,
          "audit: delete_user row survives with NULL target "
          "(ON DELETE SET NULL, history kept)");

    int root2_id = seed_user("root2", "root", 1);
    check(root2_id > 0, "seed root2");
    expect_fail(users_delete(root_id, root2_id, err, sizeof(err)),
                err, "Cannot delete root user", "root-delete refused");

    /* ── passwd ────────────────────────────────────────────────── */
    check(users_passwd(root_id, bob_id, "newpass123",
                       err, sizeof(err)) == 0, "passwd bob ok");
    check(audit_count("reset_password", bob_id) == 1,
          "audit: reset_password targets bob");
    {
        int id = -1;
        check(auth_user_login("bob", "newpass123", &id) == bob_id,
              "bob logs in with new password");
    }

    /* ── toggle invariants ─────────────────────────────────────── */
    expect_fail(users_toggle(root_id, root_id, 0, err, sizeof(err)),
                err, "Cannot disable yourself", "self-disable refused");
    check(users_toggle(root_id, bob_id, 0, err, sizeof(err)) == 0,
          "disable bob ok");
    check(audit_count("disable_user", bob_id) == 1,
          "audit: disable_user targets bob");
    {
        int id = -1;
        check(auth_user_login("bob", "newpass123", &id) == -1,
              "disabled user cannot log in");
    }
    expect_fail(users_toggle(root_id, 999, 1, err, sizeof(err)),
                err, "User not found", "toggle nonexistent refused");

    /* last-root protection (admin actor, module has no role check —
     * role lives in the gate) */
    check(users_toggle(bob_id, root2_id, 0, err, sizeof(err)) == 0,
          "admin disables root2 (root1 still enabled)");
    expect_fail(users_toggle(bob_id, root_id, 0, err, sizeof(err)),
                err, "Cannot disable last root user",
                "disabling the last enabled root refused");
    check(users_toggle(bob_id, root2_id, 1, err, sizeof(err)) == 0,
          "admin re-enables root2");

    /* ── final state ───────────────────────────────────────────── */
    check(users_fetch_all(&rows, &n) == 0 && n == 3,
          "fetch_all: root + bob + root2 (alice deleted)");
    free(rows);

    /* ── migration: v1 DB (FKs without delete actions) → v2 ─────── */
    auth_cleanup();
    remove(db_path);
    check(create_v1_db(db_path) == 0, "seed v1 schema DB");
    check(auth_init(db_path) == 0, "auth_init on v1 DB (migration runs)");
    check(strcmp(fk_action("audit_log"), "SET NULL") == 0,
          "migration: audit_log FK → ON DELETE SET NULL");
    check(strcmp(fk_action("sessions"), "CASCADE") == 0,
          "migration: sessions FK → ON DELETE CASCADE");
    check(audit_count("create_user", 1) == 1,
          "migration: audit rows preserved");
    check(table_count("sessions") == 1,
          "migration: session rows preserved");

    auth_cleanup();
    remove(db_path);

    printf("\n Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* ── v1 schema (no ON DELETE actions) + seed rows ────────────────── */
static int create_v1_db(const char *path) {
    sqlite3 *db;
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;
    sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    const char *sql =
        "CREATE TABLE users ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username      TEXT    NOT NULL UNIQUE,"
        "  password_hash TEXT    NOT NULL,"
        "  role          TEXT    NOT NULL CHECK(role IN ('root','admin')),"
        "  enabled       INTEGER NOT NULL DEFAULT 1,"
        "  created_at    INTEGER NOT NULL,"
        "  updated_at    INTEGER NOT NULL,"
        "  last_login_at INTEGER,"
        "  created_by    INTEGER,"
        "  FOREIGN KEY (created_by) REFERENCES users(id)"
        ");"
        "CREATE TABLE sessions ("
        "  sid           TEXT    PRIMARY KEY,"
        "  user_id       INTEGER NOT NULL,"
        "  csrf_token    TEXT    NOT NULL,"
        "  created_at    INTEGER NOT NULL,"
        "  expires_at    INTEGER NOT NULL,"
        "  client_ip     TEXT,"
        "  FOREIGN KEY (user_id) REFERENCES users(id)"
        ");"
        "CREATE TABLE audit_log ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  operator_id   INTEGER,"
        "  action        TEXT    NOT NULL,"
        "  target_user_id INTEGER,"
        "  detail        TEXT,"
        "  client_ip     TEXT,"
        "  created_at    INTEGER NOT NULL,"
        "  FOREIGN KEY (operator_id)    REFERENCES users(id),"
        "  FOREIGN KEY (target_user_id) REFERENCES users(id)"
        ");"
        "INSERT INTO users (username, password_hash, role, enabled, "
        "created_at, updated_at) VALUES ('root', 'x', 'root', 1, 0, 0);"
        "INSERT INTO audit_log (operator_id, action, target_user_id, "
        "detail, client_ip, created_at) VALUES (1, 'create_user', 1, "
        "'root', '', 0);"
        "INSERT INTO sessions (sid, user_id, csrf_token, created_at, "
        "expires_at, client_ip) VALUES ('s1', 1, 'c', 0, 9999999999, '');";
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}

static const char *fk_action(const char *table) {
    static char buf[16] = "";
    sqlite3 *db = auth_db();
    sqlite3_stmt *stmt;
    char pragma[64];
    snprintf(pragma, sizeof(pragma), "PRAGMA foreign_key_list(%s)", table);
    if (sqlite3_prepare_v2(db, pragma, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *tbl = (const char *)sqlite3_column_text(stmt, 2);
            if (tbl && strcmp(tbl, "users") == 0) {
                const char *od = (const char *)sqlite3_column_text(stmt, 6);
                snprintf(buf, sizeof(buf), "%s", od ? od : "");
            }
        }
        sqlite3_finalize(stmt);
    }
    return buf;
}

static int table_count(const char *table) {
    sqlite3 *db = auth_db();
    sqlite3_stmt *stmt;
    int n = -1;
    char sql[64];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            n = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return n;
}

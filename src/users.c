/* users.c — User management module: business invariants + SQL on auth_db()
 *
 * See users.h for the contract.  Error strings are kept verbatim from
 * the original user_*.cgi files so existing tests stay valid.
 * Invariant notes (ruleset approved when carved out of the CGIs):
 *   - delete: any root is immortal; self-deletion refused
 *   - toggle: self-toggle refused; disabling a root must leave at
 *     least one other enabled root (counted excluding the target,
 *     which is stricter than the old code)
 *   - the old user_delete "cannot delete last root" check was dead
 *     code (root deletion is already refused) and is removed
 */
#include "users.h"
#include "auth.h"
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define USERNAME_MIN 3
#define USERNAME_MAX 32
#define PASSWORD_MIN 6
#define PASSWORD_MAX 64

static void fail(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg);
}

int users_create(int actor_id, const char *username, const char *password,
                 char *err, size_t errlen) {
    size_t uname_len = username ? strlen(username) : 0;
    size_t pass_len  = password ? strlen(password) : 0;
    if (uname_len < USERNAME_MIN || uname_len > USERNAME_MAX ||
        pass_len  < PASSWORD_MIN || pass_len  > PASSWORD_MAX) {
        fail(err, errlen,
             "Invalid parameters (username: 3-32 chars, password: 6-64 chars)");
        return -1;
    }

    char hash[256];
    if (auth_hash_password(password, hash, sizeof(hash)) != 0) {
        fail(err, errlen, "Password hashing failed");
        return -1;
    }

    sqlite3 *db = auth_db();
    if (!db) { fail(err, errlen, "Database not open"); return -1; }

    sqlite3_stmt *stmt;
    time_t now = time(NULL);
    const char *sql =
        "INSERT INTO users (username, password_hash, role, enabled, "
        "created_at, updated_at, created_by) VALUES (?, ?, 'admin', 1, ?, ?, ?)";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fail(err, errlen, "Database error");
        return -1;
    }
    sqlite3_bind_text (stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text (stmt, 2, hash,     -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, (int64_t)now);
    sqlite3_bind_int64(stmt, 4, (int64_t)now);
    sqlite3_bind_int  (stmt, 5, actor_id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fail(err, errlen, "Username may already exist");
        return -1;
    }

    /* Audit with the real target id (old CGIs passed 0 → FK violation,
     * which silently dropped the row). */
    auth_audit_log(actor_id, "create_user",
                   (int)sqlite3_last_insert_rowid(db), username,
                   getenv("REMOTE_ADDR"));
    return 0;
}

int users_delete(int actor_id, int target_id, char *err, size_t errlen) {
    if (target_id == actor_id) {
        fail(err, errlen, "Cannot delete yourself");
        return -1;
    }

    sqlite3 *db = auth_db();
    if (!db) { fail(err, errlen, "Database not open"); return -1; }

    /* Target must exist and must not be root */
    sqlite3_stmt *stmt;
    char target_name[64] = "";
    int is_root = 0, found = 0;
    const char *role_sql = "SELECT role, username FROM users WHERE id = ?";
    if (sqlite3_prepare_v2(db, role_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, target_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            found = 1;
            strncpy(target_name,
                    (const char *)sqlite3_column_text(stmt, 1),
                    sizeof(target_name) - 1);
            is_root = (strcmp((const char *)sqlite3_column_text(stmt, 0),
                              "root") == 0);
        }
        sqlite3_finalize(stmt);
    }
    if (!found) { fail(err, errlen, "User not found or cannot be deleted"); return -1; }
    if (is_root) { fail(err, errlen, "Cannot delete root user"); return -1; }

    /* Audit BEFORE the delete: a row written after would violate the
     * target FK (target no longer exists) and be silently dropped.
     * Written first, the row survives with target SET NULL. */
    auth_audit_log(actor_id, "delete_user", target_id, target_name,
                   getenv("REMOTE_ADDR"));

    const char *del_sql = "DELETE FROM users WHERE id = ?";
    if (sqlite3_prepare_v2(db, del_sql, -1, &stmt, NULL) != SQLITE_OK) {
        fail(err, errlen, "Database error");
        return -1;
    }
    sqlite3_bind_int(stmt, 1, target_id);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE || changes <= 0) {
        fail(err, errlen, "User not found or cannot be deleted");
        return -1;
    }

    return 0;
}

int users_toggle(int actor_id, int target_id, int enabled,
                 char *err, size_t errlen) {
    if (target_id == actor_id) {
        fail(err, errlen, "Cannot disable yourself");
        return -1;
    }

    sqlite3 *db = auth_db();
    if (!db) { fail(err, errlen, "Database not open"); return -1; }

    sqlite3_stmt *stmt;
    int is_root = 0, found = 0;
    const char *role_sql = "SELECT role FROM users WHERE id = ?";
    if (sqlite3_prepare_v2(db, role_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, target_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            found = 1;
            is_root = (strcmp((const char *)sqlite3_column_text(stmt, 0),
                              "root") == 0);
        }
        sqlite3_finalize(stmt);
    }
    if (!found) { fail(err, errlen, "User not found"); return -1; }

    /* Last-root protection: disabling a root must leave another one
     * enabled.  Count excludes the target (the old code counted it,
     * misjudging re-disable of an already-disabled root). */
    if (!enabled && is_root) {
        const char *cnt_sql =
            "SELECT COUNT(*) FROM users WHERE role='root' AND enabled=1 AND id != ?";
        if (sqlite3_prepare_v2(db, cnt_sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, target_id);
            if (sqlite3_step(stmt) == SQLITE_ROW &&
                sqlite3_column_int(stmt, 0) <= 0) {
                sqlite3_finalize(stmt);
                fail(err, errlen, "Cannot disable last root user");
                return -1;
            }
            sqlite3_finalize(stmt);
        }
    }

    time_t now = time(NULL);
    const char *sql =
        "UPDATE users SET enabled = ?, updated_at = ? WHERE id = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fail(err, errlen, "Database error");
        return -1;
    }
    sqlite3_bind_int  (stmt, 1, enabled);
    sqlite3_bind_int64(stmt, 2, (int64_t)now);
    sqlite3_bind_int  (stmt, 3, target_id);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE || changes <= 0) {
        fail(err, errlen, "User not found");
        return -1;
    }

    auth_audit_log(actor_id, enabled ? "enable_user" : "disable_user",
                   target_id, "", getenv("REMOTE_ADDR"));
    return 0;
}

int users_passwd(int actor_id, int target_id, const char *password,
                 char *err, size_t errlen) {
    size_t pass_len = password ? strlen(password) : 0;
    if (pass_len < PASSWORD_MIN || pass_len > PASSWORD_MAX) {
        fail(err, errlen, "Invalid parameters");
        return -1;
    }

    char hash[256];
    if (auth_hash_password(password, hash, sizeof(hash)) != 0) {
        fail(err, errlen, "Password hashing failed");
        return -1;
    }

    sqlite3 *db = auth_db();
    if (!db) { fail(err, errlen, "Database not open"); return -1; }

    sqlite3_stmt *stmt;
    time_t now = time(NULL);
    const char *sql =
        "UPDATE users SET password_hash = ?, updated_at = ? WHERE id = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fail(err, errlen, "Database error");
        return -1;
    }
    sqlite3_bind_text (stmt, 1, hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (int64_t)now);
    sqlite3_bind_int  (stmt, 3, target_id);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE || changes <= 0) {
        fail(err, errlen, "User not found");
        return -1;
    }

    auth_audit_log(actor_id, "reset_password", target_id, "",
                   getenv("REMOTE_ADDR"));
    return 0;
}

int users_fetch_all(UserRow **out, int *count) {
    if (!out || !count) return -1;
    *out = NULL;
    *count = 0;

    sqlite3 *db = auth_db();
    if (!db) return -1;

    /* Count first */
    sqlite3_stmt *stmt;
    int n = 0;
    const char *cnt_sql = "SELECT COUNT(*) FROM users";
    if (sqlite3_prepare_v2(db, cnt_sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            n = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    UserRow *rows = calloc(n > 0 ? (size_t)n : 1, sizeof(UserRow));
    if (!rows) return -1;

    const char *sql = "SELECT id, username, role, enabled, created_at, "
                      "last_login_at FROM users ORDER BY id";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(rows);
        return -1;
    }

    int i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        UserRow *r = &rows[i++];
        r->id = sqlite3_column_int(stmt, 0);
        strncpy(r->username,
                (const char *)sqlite3_column_text(stmt, 1),
                sizeof(r->username) - 1);
        strncpy(r->role,
                (const char *)sqlite3_column_text(stmt, 2),
                sizeof(r->role) - 1);
        r->enabled = sqlite3_column_int(stmt, 3);
        r->created_at    = sqlite3_column_int64(stmt, 4);
        r->last_login_at = sqlite3_column_int64(stmt, 5);
    }
    sqlite3_finalize(stmt);

    *out = rows;
    *count = i;
    return 0;
}

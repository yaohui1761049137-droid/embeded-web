/* db_init.c — Initialize SQLite database and create root user
 *
 * Usage: ./db_init [root_password]
 * Default password: "admin"
 *
 * Compile: gcc -Wall -O2 -o db_init db_init.c auth.c sqlite3.c sha256.c -lpthread -ldl
 */
#include "auth.h"
#include "sqlite3.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DB_PATH "/var/db/myapp.db"

int main(int argc, char **argv) {
    const char *password = (argc > 1) ? argv[1] : "admin";
    char hash[256];

    printf("Initializing database: %s\n", DB_PATH);

    if (auth_init(DB_PATH) != 0) {
        fprintf(stderr, "ERROR: cannot open/create database\n");
        return 1;
    }

    /* Hash password once */
    if (auth_hash_password(password, hash, sizeof(hash)) != 0) {
        fprintf(stderr, "ERROR: password hashing failed\n");
        return 1;
    }

    /* Check if root user already exists */
    sqlite3 *db;
    sqlite3_open(DB_PATH, &db);
    sqlite3_stmt *stmt;
    int root_exists = 0;
    time_t now = time(NULL);

    const char *check_sql = "SELECT COUNT(*) FROM users WHERE role = 'root'";
    if (sqlite3_prepare_v2(db, check_sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            root_exists = sqlite3_column_int(stmt, 0) > 0;
        sqlite3_finalize(stmt);
    }

    if (root_exists) {
        printf("Root user already exists. Updating password...\n");
        const char *upd = "UPDATE users SET password_hash = ?, updated_at = ? WHERE role = 'root'";
        if (sqlite3_prepare_v2(db, upd, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text (stmt, 1, hash, -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 2, (int64_t)now);
            int rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            printf("%s\n", rc == SQLITE_DONE ? "Root password updated." : "Update failed.");
        }
    } else {
        printf("Creating root user...\n");
        const char *ins =
            "INSERT INTO users (username, password_hash, role, enabled, "
            "created_at, updated_at) VALUES ('root', ?, 'root', 1, ?, ?)";
        if (sqlite3_prepare_v2(db, ins, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text (stmt, 1, hash, -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 2, (int64_t)now);
            sqlite3_bind_int64(stmt, 3, (int64_t)now);
            if (sqlite3_step(stmt) == SQLITE_DONE)
                printf("Root user created.\n");
            else
                fprintf(stderr, "ERROR: %s\n", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
        }
    }

    sqlite3_close(db);
    auth_cleanup();

    printf("Password hash: %s\n", hash);
    printf("Done.\n");
    return 0;
}

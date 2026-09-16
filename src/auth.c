/* auth.c — Multi-user authentication library for LubanCat
 *
 * SQLite-backed session/user/CSRF management.
 * Password hashing: SHA-256 with salt + 100k iterations.
 *
 * Compile together with: sqlite3.c sha256.c common.c (modified)
 */

#include "auth.h"
#include "sha256.h"
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

/* ── Internal state ───────────────────────────────────────────────── */
static sqlite3 *g_db = NULL;
static char     g_db_path[256] = {0};

static void migrate_delete_fks(sqlite3 *db);
static void migrate_password_policy(sqlite3 *db);

/* ── Database init / cleanup ──────────────────────────────────────── */

int auth_init(const char *db_path) {
    int rc;

    if (g_db) {
        /* Already open — check if same path */
        if (strcmp(g_db_path, db_path) == 0) return 0;
        auth_cleanup();
    }

    rc = sqlite3_open(db_path, &g_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "auth_init: cannot open %s: %s\n",
                db_path, sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    strncpy(g_db_path, db_path, sizeof(g_db_path) - 1);
    g_db_path[sizeof(g_db_path) - 1] = '\0';

    /* Concurrency settings */
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;",      NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL;",     NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA busy_timeout=5000;",      NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA foreign_keys=ON;",        NULL, NULL, NULL);

    /* Create tables if not exist */
    const char *sql =
        "CREATE TABLE IF NOT EXISTS users ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username      TEXT    NOT NULL UNIQUE,"
        "  password_hash TEXT    NOT NULL,"
        "  role          TEXT    NOT NULL CHECK(role IN ('root','admin')),"
        "  enabled       INTEGER NOT NULL DEFAULT 1,"
        "  created_at    INTEGER NOT NULL,"
        "  updated_at    INTEGER NOT NULL,"
        "  last_login_at INTEGER,"
        "  created_by    INTEGER,"
        "  password_changed_at INTEGER NOT NULL DEFAULT 0,"
        "  FOREIGN KEY (created_by) REFERENCES users(id)"
        "  FOREIGN KEY (created_by) REFERENCES users(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  sid           TEXT    PRIMARY KEY,"
        "  user_id       INTEGER NOT NULL,"
        "  csrf_token    TEXT    NOT NULL,"
        "  created_at    INTEGER NOT NULL,"
        "  expires_at    INTEGER NOT NULL,"
        "  client_ip     TEXT,"
        "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
        ");"
        "CREATE TABLE IF NOT EXISTS audit_log ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  operator_id   INTEGER,"
        "  action        TEXT    NOT NULL,"
        "  target_user_id INTEGER,"
        "  detail        TEXT,"
        "  client_ip     TEXT,"
        "  created_at    INTEGER NOT NULL,"
        "  FOREIGN KEY (operator_id)    REFERENCES users(id) ON DELETE SET NULL,"
        "  FOREIGN KEY (target_user_id) REFERENCES users(id) ON DELETE SET NULL"
        ");";

    rc = sqlite3_exec(g_db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "auth_init: cannot create tables: %s\n",
                sqlite3_errmsg(g_db));
        auth_cleanup();
        return -1;
    }

    migrate_delete_fks(g_db);
    migrate_password_policy(g_db);

    return 0;
}

/* ADR-0002: legacy users tables lack password_changed_at; add it so
 * pre-policy accounts are treated as expired (0) → forced change on
 * next login.  Probe with PRAGMA table_info instead of an unconditional
 * ALTER (which would fail once the column exists). */
static void migrate_password_policy(sqlite3 *db) {
    sqlite3_stmt *stmt;
    int found = 0;
    const char *sql = "PRAGMA table_info(users)";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(stmt, 1);
            if (name && strcmp(name, "password_changed_at") == 0) {
                found = 1;
                break;
            }
        }
        sqlite3_finalize(stmt);
    }

    if (!found) {
        sqlite3_exec(db,
            "ALTER TABLE users ADD COLUMN password_changed_at "
            "INTEGER NOT NULL DEFAULT 0",
            NULL, NULL, NULL);
    }
}

/* ── Schema migration ─────────────────────────────────────────────── */

/* v1 schema declared audit_log/sessions FKs without ON DELETE actions,
 * so deleting a user with audit history or live sessions was blocked
 * by the FK constraints (user_delete silently failed).  Rebuild those
 * tables with proper delete actions when the old schema is detected:
 * audit history survives with NULL target, sessions are cascaded.
 * Runs once — after the rebuild the PRAGMA no longer matches. */
static void migrate_delete_fks(sqlite3 *db) {
    /* PRAGMA foreign_key_list columns: id, seq, table, from, to,
     * on_update, on_delete, match */
    sqlite3_stmt *stmt;
    int audit_needs = 0, sess_needs = 0;
    const char *sql = "PRAGMA foreign_key_list(audit_log)";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *tbl = (const char *)sqlite3_column_text(stmt, 2);
            const char *od  = (const char *)sqlite3_column_text(stmt, 6);
            if (tbl && strcmp(tbl, "users") == 0 &&
                (!od || strcmp(od, "SET NULL") != 0))
                audit_needs = 1;
        }
        sqlite3_finalize(stmt);
    }

    sql = "PRAGMA foreign_key_list(sessions)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *tbl = (const char *)sqlite3_column_text(stmt, 2);
            const char *od  = (const char *)sqlite3_column_text(stmt, 6);
            if (tbl && strcmp(tbl, "users") == 0 &&
                (!od || strcmp(od, "CASCADE") != 0))
                sess_needs = 1;
        }
        sqlite3_finalize(stmt);
    }

    if (audit_needs) {
        /* A previously aborted run may have left a half-built v2 table;
         * drop it first so a retry always succeeds (a stale leftover
         * otherwise made every future attempt fail silently on
         * "table audit_log_v2 already exists"). */
        char *emsg = NULL;
        sqlite3_exec(db,
            "BEGIN;"
            "DROP TABLE IF EXISTS audit_log_v2;"
            "CREATE TABLE audit_log_v2 ("
            "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  operator_id   INTEGER,"
            "  action        TEXT    NOT NULL,"
            "  target_user_id INTEGER,"
            "  detail        TEXT,"
            "  client_ip     TEXT,"
            "  created_at    INTEGER NOT NULL,"
            "  FOREIGN KEY (operator_id)    REFERENCES users(id) ON DELETE SET NULL,"
            "  FOREIGN KEY (target_user_id) REFERENCES users(id) ON DELETE SET NULL"
            ");"
            "INSERT INTO audit_log_v2 SELECT id, operator_id, action, "
            "target_user_id, detail, client_ip, created_at FROM audit_log;"
            "DROP TABLE audit_log;"
            "ALTER TABLE audit_log_v2 RENAME TO audit_log;"
            "COMMIT;", NULL, NULL, &emsg);
        if (emsg) {
            fprintf(stderr, "auth: audit_log migration failed: %s\n", emsg);
            sqlite3_free(emsg);
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        }
    }

    if (sess_needs) {
        char *emsg = NULL;
        sqlite3_exec(db,
            "BEGIN;"
            "DROP TABLE IF EXISTS sessions_v2;"
            "CREATE TABLE sessions_v2 ("
            "  sid           TEXT    PRIMARY KEY,"
            "  user_id       INTEGER NOT NULL,"
            "  csrf_token    TEXT    NOT NULL,"
            "  created_at    INTEGER NOT NULL,"
            "  expires_at    INTEGER NOT NULL,"
            "  client_ip     TEXT,"
            "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
            ");"
            "INSERT INTO sessions_v2 SELECT sid, user_id, csrf_token, "
            "created_at, expires_at, client_ip FROM sessions;"
            "DROP TABLE sessions;"
            "ALTER TABLE sessions_v2 RENAME TO sessions;"
            "COMMIT;", NULL, NULL, &emsg);
        if (emsg) {
            fprintf(stderr, "auth: sessions migration failed: %s\n", emsg);
            sqlite3_free(emsg);
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        }
    }
}

void auth_cleanup(void) {
    if (g_db) {
        /* Clean expired sessions on every close */
        auth_session_cleanup();
        sqlite3_close(g_db);
        g_db = NULL;
        g_db_path[0] = '\0';
    }
}

sqlite3 *auth_db(void) {
    return g_db;
}

/* ── Token generation ────────────────────────────────────────────── */

void auth_generate_token(char *out, int len) {
    static const char hex[] = "0123456789abcdef";
    unsigned char buf[len / 2];
    int fd, i;

    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        /* Fallback: very weak but won't crash */
        for (i = 0; i < len / 2; i++) buf[i] = (unsigned char)(rand() ^ time(NULL));
    } else {
        ssize_t n = read(fd, buf, len / 2);
        if (n < len / 2) {
            for (i = n; i < (int)(len / 2); i++) buf[i] = 0;
        }
        close(fd);
    }

    for (i = 0; i < len / 2; i++) {
        out[i * 2]     = hex[buf[i] >> 4];
        out[i * 2 + 1] = hex[buf[i] & 0x0f];
    }
    out[len] = '\0';
}

/* ── Session management ──────────────────────────────────────────── */

int auth_session_create(int user_id, const char *client_ip,
                         char *sid_out, char *csrf_out) {
    char sid[SESSION_ID_LEN + 1];
    char csrf[CSRF_TOKEN_LEN + 1];
    time_t now = time(NULL);
    sqlite3_stmt *stmt;
    int rc;

    if (!g_db) return 0;

    auth_generate_token(sid,  SESSION_ID_LEN);
    auth_generate_token(csrf, CSRF_TOKEN_LEN);

    const char *sql =
        "INSERT INTO sessions (sid, user_id, csrf_token, created_at, "
        "expires_at, client_ip) VALUES (?, ?, ?, ?, ?, ?)";

    rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_text (stmt, 1, sid,  -1, SQLITE_STATIC);
    sqlite3_bind_int  (stmt, 2, user_id);
    sqlite3_bind_text (stmt, 3, csrf, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, (int64_t)now);
    sqlite3_bind_int64(stmt, 5, (int64_t)(now + SESSION_EXPIRE));
    sqlite3_bind_text (stmt, 6, client_ip ? client_ip : "", -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) return 0;

    if (sid_out)  memcpy(sid_out,  sid,  SESSION_ID_LEN + 1);
    if (csrf_out) memcpy(csrf_out, csrf, CSRF_TOKEN_LEN + 1);

    /* Update last_login_at */
    sql = "UPDATE users SET last_login_at = ? WHERE id = ?";
    sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, (int64_t)now);
    sqlite3_bind_int  (stmt, 2, user_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return 1;
}

int auth_session_verify(const char *sid, SessionInfo *out) {
    sqlite3_stmt *stmt;
    int found = 0;

    if (!g_db || !sid || strlen(sid) != SESSION_ID_LEN) return 0;

    const char *sql =
        "SELECT s.user_id, u.username, u.role, s.csrf_token, u.enabled "
        "FROM sessions s JOIN users u ON s.user_id = u.id "
        "WHERE s.sid = ? AND s.expires_at > ?";

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_text (stmt, 1, sid, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (int64_t)time(NULL));

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        /* Check user is enabled */
        if (sqlite3_column_int(stmt, 4) == 0) {
            sqlite3_finalize(stmt);
            return 0;
        }

        if (out) {
            memset(out, 0, sizeof(*out));
            out->user_id = sqlite3_column_int(stmt, 0);
            strncpy(out->username,
                    (const char *)sqlite3_column_text(stmt, 1),
                    sizeof(out->username) - 1);
            strncpy(out->role,
                    (const char *)sqlite3_column_text(stmt, 2),
                    sizeof(out->role) - 1);
            strncpy(out->csrf_token,
                    (const char *)sqlite3_column_text(stmt, 3),
                    sizeof(out->csrf_token) - 1);
        }
        found = 1;
    }

    sqlite3_finalize(stmt);
    return found;
}

void auth_session_destroy(const char *sid) {
    sqlite3_stmt *stmt;
    if (!g_db || !sid) return;

    const char *sql = "DELETE FROM sessions WHERE sid = ?";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void auth_session_cleanup(void) {
    if (!g_db) return;
    const char *sql = "DELETE FROM sessions WHERE expires_at < ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

/* ── Password hashing (SHA-256, salted, iterated) ─────────────────── */

int auth_hash_password(const char *password, char *hash_out, int max_hash) {
    unsigned char salt[PASSWORD_SALT_LEN];
    unsigned char digest[SHA256_DIGEST_SIZE];
    unsigned char combined[128];
    sha256_ctx ctx;
    int pwlen, saltlen;
    int fd, i, round;
    char salt_hex[PASSWORD_SALT_LEN * 2 + 1];
    char hash_hex[SHA256_DIGEST_SIZE * 2 + 1];
    static const char hx[] = "0123456789abcdef";

    if (!password || !hash_out || max_hash < 128) return -1;

    pwlen = strlen(password);
    if (pwlen < 1 || pwlen > 64) return -1;

    /* Generate random salt */
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        for (i = 0; i < PASSWORD_SALT_LEN; i++)
            salt[i] = (unsigned char)(rand() ^ time(NULL));
    } else {
        ssize_t n = read(fd, salt, PASSWORD_SALT_LEN);
        if (n < PASSWORD_SALT_LEN) {
            for (i = n; i < PASSWORD_SALT_LEN; i++) salt[i] = 0;
        }
        close(fd);
    }

    /* Hex-encode salt */
    for (i = 0; i < PASSWORD_SALT_LEN; i++) {
        salt_hex[i*2]     = hx[salt[i] >> 4];
        salt_hex[i*2 + 1] = hx[salt[i] & 0x0f];
    }
    salt_hex[PASSWORD_SALT_LEN * 2] = '\0';
    saltlen = PASSWORD_SALT_LEN * 2;

    /* Build combined input: salt_hex + ":" + password */
    memcpy(combined, salt_hex, saltlen);
    combined[saltlen] = ':';
    memcpy(combined + saltlen + 1, password, pwlen);
    int combined_len = saltlen + 1 + pwlen;

    /* First hash */
    sha256_ctx first_ctx;
    sha256_init(&first_ctx);
    sha256_update(&first_ctx, combined, combined_len);
    sha256_final(&first_ctx, digest);

    /* Iterate */
    for (round = 1; round < PASSWORD_ROUNDS; round++) {
        sha256_init(&ctx);
        sha256_update(&ctx, digest, SHA256_DIGEST_SIZE);
        sha256_update(&ctx, (uint8_t *)&round, sizeof(round));
        sha256_final(&ctx, digest);
    }

    /* Hex-encode final digest */
    for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
        hash_hex[i*2]     = hx[digest[i] >> 4];
        hash_hex[i*2 + 1] = hx[digest[i] & 0x0f];
    }
    hash_hex[SHA256_DIGEST_SIZE * 2] = '\0';

    /* Format: $5$rounds=N$<salt_hex>$<hash_hex> */
    int written = snprintf(hash_out, max_hash, "$5$rounds=%d$%s$%s",
                           PASSWORD_ROUNDS, salt_hex, hash_hex);
    return (written > 0 && written < max_hash) ? 0 : -1;
}

int auth_verify_password(const char *password, const char *stored_hash) {
    char salt_hex[PASSWORD_SALT_LEN * 2 + 1];
    unsigned char digest[SHA256_DIGEST_SIZE];
    unsigned char combined[128];
    sha256_ctx ctx;
    int rounds, pwlen, saltlen, round, i;
    const char *p;
    char hash_hex[SHA256_DIGEST_SIZE * 2 + 1];
    static const char hx[] = "0123456789abcdef";

    if (!password || !stored_hash) return 0;
    pwlen = strlen(password);
    if (pwlen < 1 || pwlen > 64) return 0;

    /* Parse: $5$rounds=N$<salt_hex>$<hash_hex> */
    if (strncmp(stored_hash, "$5$rounds=", 10) != 0) return 0;
    p = stored_hash + 10;
    rounds = atoi(p);

    /* Skip to salt */
    p = strchr(p, '$');
    if (!p) return 0;
    p++; /* now points to salt */

    /* Extract salt (hex) */
    const char *salt_start = p;
    p = strchr(p, '$');
    if (!p) return 0;
    saltlen = (int)(p - salt_start);
    if (saltlen != PASSWORD_SALT_LEN * 2) return 0;
    memcpy(salt_hex, salt_start, saltlen);
    salt_hex[saltlen] = '\0';

    /* Build combined input: salt_hex + ":" + password */
    memcpy(combined, salt_hex, saltlen);
    combined[saltlen] = ':';
    memcpy(combined + saltlen + 1, password, pwlen);
    int combined_len = saltlen + 1 + pwlen;

    /* First hash */
    sha256_ctx first_ctx;
    sha256_init(&first_ctx);
    sha256_update(&first_ctx, combined, combined_len);
    sha256_final(&first_ctx, digest);

    /* Iterate */
    for (round = 1; round < rounds; round++) {
        sha256_init(&ctx);
        sha256_update(&ctx, digest, SHA256_DIGEST_SIZE);
        sha256_update(&ctx, (uint8_t *)&round, sizeof(round));
        sha256_final(&ctx, digest);
    }

    /* Hex-encode */
    for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
        hash_hex[i*2]     = hx[digest[i] >> 4];
        hash_hex[i*2 + 1] = hx[digest[i] & 0x0f];
    }
    hash_hex[SHA256_DIGEST_SIZE * 2] = '\0';

    /* Compare with stored hash (the part after the last $) */
    const char *stored_digest = p + 1; /* skip the $ after salt */
    return (strcmp(hash_hex, stored_digest) == 0) ? 1 : 0;
}

/* ── User login ──────────────────────────────────────────────────── */

int auth_user_login(const char *username, const char *password,
                     int *user_id_out) {
    sqlite3_stmt *stmt;
    int user_id = -1;

    if (!g_db || !username || !password) return -1;

    const char *sql =
        "SELECT id, password_hash FROM users "
        "WHERE username = ? AND enabled = 1";

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const char *stored_hash =
            (const char *)sqlite3_column_text(stmt, 1);

        if (auth_verify_password(password, stored_hash)) {
            user_id = id;
        }
    }

    sqlite3_finalize(stmt);

    if (user_id_out) *user_id_out = user_id;
    return user_id;
}

/* ── Password policy (ADR-0002) ──────────────────────────────────── */

int auth_password_policy_ok(const char *password, char *err, int err_max) {
    int len, i;
    int has_upper = 0, has_lower = 0, has_digit = 0, has_punct = 0;

    if (!password) return 0;
    len = strlen(password);

    if (len < PASSWORD_MIN_LEN || len > PASSWORD_MAX_LEN) {
        if (err) snprintf(err, err_max, "密码长度需为 %d-%d 个字符",
                          PASSWORD_MIN_LEN, PASSWORD_MAX_LEN);
        return 0;
    }

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)password[i];
        if (c < 0x21 || c > 0x7E) {   /* control chars, space, non-ASCII */
            if (err) snprintf(err, err_max,
                              "密码只能包含 ASCII 可见字符(不允许中文、空格)");
            return 0;
        }
        if (c >= 'A' && c <= 'Z')      has_upper = 1;
        else if (c >= 'a' && c <= 'z') has_lower = 1;
        else if (c >= '0' && c <= '9') has_digit = 1;
        else                           has_punct = 1;  /* ASCII punct 0x21-0x7E */
    }

    if (!has_upper || !has_lower || !has_digit || !has_punct) {
        if (err) snprintf(err, err_max,
                          "密码需包含大写字母、小写字母、数字和特殊符号");
        return 0;
    }
    return 1;
}

/* Read users.password_changed_at; returns 1 if found, 0 otherwise. */
static int auth_user_password_changed_at(int user_id, int64_t *out) {
    sqlite3_stmt *stmt;

    if (!g_db || user_id <= 0) return 0;

    if (sqlite3_prepare_v2(g_db,
            "SELECT password_changed_at FROM users WHERE id = ?",
            -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_int(stmt, 1, user_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return 1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

int auth_user_must_change_password(int user_id) {
    int64_t changed_at;
    if (!auth_user_password_changed_at(user_id, &changed_at)) return 0;
    if (changed_at == 0) return 1;                     /* legacy account */
    return (time(NULL) - changed_at >=
            (int64_t)PASSWORD_EXPIRE_DAYS * 86400LL) ? 1 : 0;
}

int auth_user_days_left(int user_id) {
    int64_t changed_at, days;
    if (!auth_user_password_changed_at(user_id, &changed_at)) return -1;
    if (changed_at == 0) return 0;
    days = ((int64_t)PASSWORD_EXPIRE_DAYS * 86400LL
            - (time(NULL) - changed_at)) / 86400;
    return days < 0 ? 0 : (int)days;
}

void auth_kick_user_sessions(int user_id, const char *keep_sid) {
    sqlite3_stmt *stmt;
    if (!g_db || user_id <= 0) return;

    const char *sql = keep_sid
        ? "DELETE FROM sessions WHERE user_id = ? AND sid != ?"
        : "DELETE FROM sessions WHERE user_id = ?";

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, user_id);
        if (keep_sid) sqlite3_bind_text(stmt, 2, keep_sid, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

/* ── Permissions ─────────────────────────────────────────────────── */

int auth_require_role(const SessionInfo *s, const char *role) {
    if (!s || !role) return 0;
    /* root can do anything */
    if (strcmp(s->role, "root") == 0) return 1;
    return (strcmp(s->role, role) == 0) ? 1 : 0;
}

/* ── CSRF ────────────────────────────────────────────────────────── */

int auth_csrf_verify(const SessionInfo *s, const char *token) {
    if (!s || !token) return 0;
    return (strcmp(s->csrf_token, token) == 0) ? 1 : 0;
}

/* ── Audit ───────────────────────────────────────────────────────── */

void auth_audit_log(int operator_id, const char *action,
                    int target_user_id, const char *detail,
                    const char *client_ip) {
    sqlite3_stmt *stmt;
    if (!g_db) return;

    const char *sql =
        "INSERT INTO audit_log (operator_id, action, target_user_id, "
        "detail, client_ip, created_at) VALUES (?, ?, ?, ?, ?, ?)";

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int  (stmt, 1, operator_id);
        sqlite3_bind_text (stmt, 2, action, -1, SQLITE_STATIC);
        sqlite3_bind_int  (stmt, 3, target_user_id);
        sqlite3_bind_text (stmt, 4, detail ? detail : "", -1, SQLITE_STATIC);
        sqlite3_bind_text (stmt, 5, client_ip ? client_ip : "", -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 6, (int64_t)time(NULL));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

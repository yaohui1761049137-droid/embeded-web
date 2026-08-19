/* login.cgi — Phase 2: SQLite-backed multi-user login
 *
 * POST: verify credentials via auth.c, create session, set dual cookies.
 * GET:  check existing session, redirect to main if valid.
 */
#include "common.h"
#include "auth.h"
#include "gate.h"

/* ADR-0003 D10: a successful web login confirms any pending eth0
 * rollback (keeps the new config, cancels the watchdog).  Best-effort:
 * no file means nothing is pending.  Atomic via tmp+rename so the
 * watchdog never observes a half-written state. */
static void confirm_rollback(void) {
    const char *path = getenv("ROLLBACK_FILE");
    if (!path || !*path) path = "/var/db/rollback.json";
    FILE *f = fopen(path, "r");
    if (!f) return;

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *t = fopen(tmp_path, "w");
    if (!t) { fclose(f); return; }

    char line[256];
    int changed = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "confirmed=0", 11) == 0) {
            fputs("confirmed=1\n", t);
            changed = 1;
        } else {
            fputs(line, t);
        }
    }
    fclose(f);
    fclose(t);
    if (changed) rename(tmp_path, path);
    else         remove(tmp_path);
}

int main(void) {
    const char *method = get_env("REQUEST_METHOD");

    /* ── GET: check if already logged in ────────────────────────── */
    if (strcmp(method, "POST") != 0) {
        SessionInfo session;
        if (gate_page_session(&session)) {
            auth_cleanup();
            cgi_redirect("/cgi-bin/main.cgi");
        }
        /* 未登录：gate 已重定向到 /index.html */
        return 0;
    }

    /* ── POST: process login ────────────────────────────────────── */
    if (auth_init(gate_db_path()) != 0) {
        cgi_header("application/json; charset=utf-8");
        printf("{\"status\":\"error\",\"message\":\"Database error\"}");
        return 0;
    }

    char *user = get_post_param("user");
    char *pass = get_post_param("pass");

    if (!user || !pass || !*user || !*pass) {
        cgi_header("application/json; charset=utf-8");
        printf("{\"status\":\"error\",\"message\":\"Missing credentials\"}");
        auth_cleanup();
        return 0;
    }

    int user_id = -1;
    if (auth_user_login(user, pass, &user_id) < 0) {
        cgi_header("application/json; charset=utf-8");
        printf("{\"status\":\"error\",\"message\":\"Invalid credentials\"}");
        auth_cleanup();
        return 0;
    }

    /* Create session */
    const char *client_ip = getenv("REMOTE_ADDR");
    char sid[SESSION_ID_LEN + 1]   = {0};
    char csrf[CSRF_TOKEN_LEN + 1]  = {0};

    if (!auth_session_create(user_id, client_ip, sid, csrf)) {
        cgi_header("application/json; charset=utf-8");
        printf("{\"status\":\"error\",\"message\":\"Session creation failed\"}");
        auth_cleanup();
        return 0;
    }

    confirm_rollback();

    /* ADR-0002: password expired/never set → forced change first */
    const char *dest = auth_user_must_change_password(user_id)
        ? "/change.html" : "/cgi-bin/main.cgi";

    /* Dual cookies: session_id=HttpOnly, csrf_token=JS-readable */
    printf("Status: 302\r\n");
    gate_emit_session_cookies(sid, csrf);
    printf("Location: %s\r\n\r\n", dest);

    auth_cleanup();
    return 0;
}

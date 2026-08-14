/* login.cgi — Phase 2: SQLite-backed multi-user login
 *
 * POST: verify credentials via auth.c, create session, set dual cookies.
 * GET:  check existing session, redirect to main if valid.
 */
#include "common.h"
#include "auth.h"
#include "gate.h"

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

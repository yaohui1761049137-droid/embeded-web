/* login.cgi — Phase 2: SQLite-backed multi-user login
 *
 * POST: verify credentials via auth.c, create session, set dual cookies.
 * GET:  check existing session, redirect to main if valid.
 */
#include "common.h"
#include "auth.h"

#define DB_PATH "/var/db/myapp.db"

int main(void) {
    const char *method = get_env("REQUEST_METHOD");

    auth_init(DB_PATH);

    /* ── GET: check if already logged in ────────────────────────── */
    if (strcmp(method, "POST") != 0) {
        const char *sid = get_cookie(SESSION_COOKIE_NAME);
        SessionInfo session;
        if (sid && auth_session_verify(sid, &session)) {
            cgi_redirect("/cgi-bin/main.cgi");
        } else {
            cgi_redirect("/index.html");
        }
        auth_cleanup();
        return 0;
    }

    /* ── POST: process login ────────────────────────────────────── */
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

    /* Dual cookies: session_id=HttpOnly, csrf_token=JS-readable */
    printf("Status: 302\r\n");
    printf("Set-Cookie: %s=%s; Path=/; HttpOnly; Secure; "
           "SameSite=Lax; Max-Age=%d\r\n",
           SESSION_COOKIE_NAME, sid, SESSION_EXPIRE);
    printf("Set-Cookie: csrf_token=%s; Path=/; Secure; "
           "SameSite=Lax; Max-Age=%d\r\n",
           csrf, SESSION_EXPIRE);
    printf("Location: /cgi-bin/main.cgi\r\n\r\n");

    auth_cleanup();
    return 0;
}

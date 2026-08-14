/* gate.c — CGI request gate implementation
 *
 * Concentrates the session → role → CSRF ladder that used to be hand-rolled
 * in every CGI (with drifted orderings and a cleanup leak in action.cgi).
 */
#include "gate.h"
#include "common.h"

const char *gate_db_path(void) {
    const char *p = getenv("DB_PATH");
    return (p && *p) ? p : DB_PATH_DEFAULT;
}

static void emit_not_authenticated(int page_mode) {
    if (page_mode) {
        cgi_redirect("/index.html");
    } else {
        cgi_header("application/json; charset=utf-8");
        printf("{\"status\":\"error\",\"message\":\"Not authenticated\"}");
    }
}

static int gate_session(SessionInfo *out, int page_mode, int skip_policy) {
    const char *sid = get_cookie(SESSION_COOKIE_NAME);

    /* No cookie: reject without touching the DB (cheap path) */
    if (!sid) {
        emit_not_authenticated(page_mode);
        return 0;
    }

    if (auth_init(gate_db_path()) != 0) {
        /* auth_init failure: nothing to clean up */
        cgi_header("application/json; charset=utf-8");
        printf("{\"status\":\"error\",\"message\":\"Database error\"}");
        return 0;
    }

    SessionInfo s;
    if (!auth_session_verify(sid, &s)) {
        auth_cleanup();
        emit_not_authenticated(page_mode);
        return 0;
    }

    /* ADR-0002: expired/never-set password → forced change first.
     * Everything except the change-password CGI is blocked here. */
    if (!skip_policy && auth_user_must_change_password(s.user_id)) {
        auth_cleanup();
        if (page_mode) {
            cgi_redirect("/change.html");
        } else {
            cgi_header("application/json; charset=utf-8");
            printf("{\"status\":\"error\",\"message\":\"密码已过期,请先修改密码\"}");
        }
        return 0;
    }

    if (out) *out = s;
    return 1;
}

int gate_json_session(SessionInfo *out) {
    return gate_session(out, 0, 0);
}

int gate_page_session(SessionInfo *out) {
    return gate_session(out, 1, 0);
}

/* Session gate that skips the forced-change check — only the
 * change-password CGI may use this (ADR-0002). */
int gate_json_session_no_policy(SessionInfo *out) {
    return gate_session(out, 0, 1);
}

int gate_require_role(const SessionInfo *s, const char *role) {
    if (auth_require_role(s, role)) return 1;
    cgi_header("application/json; charset=utf-8");
    printf("{\"status\":\"error\",\"message\":\"Forbidden\"}");
    return 0;
}

int gate_require_csrf(const SessionInfo *s, const char *token) {
    if (auth_csrf_verify(s, token)) return 1;
    cgi_header("application/json; charset=utf-8");
    printf("{\"status\":\"error\",\"message\":\"CSRF token invalid\"}");
    return 0;
}

void gate_emit_session_cookies(const char *sid, const char *csrf) {
    if (sid) {
        printf("Set-Cookie: %s=%s; Path=/; HttpOnly; Secure; "
               "SameSite=Lax; Max-Age=%d\r\n",
               SESSION_COOKIE_NAME, sid, SESSION_EXPIRE);
        printf("Set-Cookie: csrf_token=%s; Path=/; Secure; "
               "SameSite=Lax; Max-Age=%d\r\n",
               csrf ? csrf : "", SESSION_EXPIRE);
    } else {
        printf("Set-Cookie: %s=; Path=/; HttpOnly; Secure; SameSite=Lax; "
               "Max-Age=0\r\n", SESSION_COOKIE_NAME);
        printf("Set-Cookie: csrf_token=; Path=/; Secure; SameSite=Lax; "
               "Max-Age=0\r\n");
    }
}

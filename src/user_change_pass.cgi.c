/* user_change_pass.cgi — ADR-0002: self-service password change
 *
 * Any authenticated user changes their own password:
 *   - requires current password (anti session-hijack)
 *   - new password must satisfy the strong policy and differ from old
 *   - restarts the 90-day validity timer
 *   - kicks the user's other sessions (keeps the current one)
 *
 * NOT subject to the forced-change gate (must stay reachable when the
 * password is expired), but still requires a valid session + CSRF.
 */
#include "common.h"
#include "auth.h"
#include "gate.h"
#include "users.h"

int main(void) {
    /* Request gate without the forced-change step (we ARE the escape
     * hatch). Session + CSRF still enforced. */
    SessionInfo session;
    if (!gate_json_session_no_policy(&session)) return 0;

    const char *csrf = get_post_param("csrf_token");
    if (!gate_require_csrf(&session, csrf)) {
        auth_cleanup();
        return 0;
    }

    const char *old_pass = get_post_param("old_password");
    const char *new_pass = get_post_param("new_password");
    if (!old_pass || !new_pass) {
        cgi_header("application/json; charset=utf-8");
        printf("{\"status\":\"error\",\"message\":\"Missing parameters\"}");
        auth_cleanup();
        return 0;
    }

    /* keep_sid: the session that made the change survives */
    const char *sid = get_cookie(SESSION_COOKIE_NAME);

    char err[128] = "";
    int rc = users_change_password(session.user_id, old_pass, new_pass,
                                   sid, err, sizeof(err));

    cgi_header("application/json; charset=utf-8");
    if (rc == 0) {
        printf("{\"status\":\"ok\",\"message\":\"Password updated\"}");
    } else {
        printf("{\"status\":\"error\",\"message\":\"%s\"}", err);
    }

    auth_cleanup();
    return 0;
}

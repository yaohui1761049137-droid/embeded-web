/* user_passwd.cgi — [root] reset user password (POST) */
#include "common.h"
#include "auth.h"
#include "gate.h"
#include "users.h"

int main(void) {
    /* Request gate: session → role → CSRF ladder (emits errors on failure) */
    SessionInfo session;
    if (!gate_json_session(&session)) return 0;

    if (!gate_require_role(&session, "root")) {
        auth_cleanup();
        return 0;
    }

    const char *csrf = get_post_param("csrf_token");
    if (!gate_require_csrf(&session, csrf)) {
        auth_cleanup();
        return 0;
    }

    const char *id_str = get_post_param("user_id");
    const char *new_pass = get_post_param("password");
    if (!id_str || !new_pass) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Invalid parameters\"}");
        auth_cleanup();
        return 0;
    }

    char err[128] = "";
    int rc = users_passwd(session.user_id, atoi(id_str), new_pass,
                          err, sizeof(err));

    cgi_header("application/json");
    if (rc == 0) {
        printf("{\"status\":\"ok\",\"message\":\"Password updated\"}");
    } else {
        printf("{\"status\":\"error\",\"message\":\"%s\"}", err);
    }

    auth_cleanup();
    return 0;
}

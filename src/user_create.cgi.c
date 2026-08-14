/* user_create.cgi — [root] create admin user (POST) */
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

    char err[128] = "";
    int rc = users_create(session.user_id,
                          get_post_param("username"),
                          get_post_param("password"),
                          err, sizeof(err));

    cgi_header("application/json");
    if (rc == 0) {
        printf("{\"status\":\"ok\",\"message\":\"User created\"}");
    } else {
        printf("{\"status\":\"error\",\"message\":\"%s\"}", err);
    }

    auth_cleanup();
    return 0;
}

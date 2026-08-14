/* user_list.cgi — [root] list users (GET)
 * Returns JSON array of users (excludes password_hash).
 */
#include "common.h"
#include "auth.h"
#include "gate.h"
#include "users.h"

int main(void) {
    /* Request gate: session → role ladder (emits errors on failure) */
    SessionInfo session;
    if (!gate_json_session(&session)) return 0;

    if (!gate_require_role(&session, "root")) {
        auth_cleanup();
        return 0;
    }

    UserRow *rows = NULL;
    int n = 0;

    cgi_header("application/json; charset=utf-8");
    printf("{\"status\":\"ok\",\"users\":[");
    if (users_fetch_all(&rows, &n) == 0) {
        for (int i = 0; i < n; i++) {
            if (i) printf(",");
            printf("{\"id\":%d,\"username\":\"%s\",\"role\":\"%s\","
                   "\"enabled\":%d,\"created_at\":%lld,\"last_login_at\":%lld}",
                   rows[i].id, rows[i].username, rows[i].role, rows[i].enabled,
                   (long long)rows[i].created_at, (long long)rows[i].last_login_at);
        }
    }
    printf("]}");
    free(rows);

    auth_cleanup();
    return 0;
}

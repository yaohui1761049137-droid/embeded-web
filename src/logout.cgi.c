/* logout.cgi — Phase 2: destroy DB session, clear both cookies */
#include "common.h"
#include "auth.h"
#include "gate.h"

int main(void) {
    auth_init(gate_db_path());

    char *sid = get_cookie(SESSION_COOKIE_NAME);
    if (sid) {
        auth_session_destroy(sid);
    }

    auth_cleanup();

    /* Clear both cookies */
    printf("Status: 302\r\n");
    gate_emit_session_cookies(NULL, NULL);
    printf("Location: /index.html\r\n\r\n");
    return 0;
}

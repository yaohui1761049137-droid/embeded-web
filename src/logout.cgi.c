/* logout.cgi — Phase 2: destroy DB session, clear both cookies */
#include "common.h"
#include "auth.h"

#define DB_PATH "/var/db/myapp.db"

int main(void) {
    auth_init(DB_PATH);

    char *sid = get_cookie(SESSION_COOKIE_NAME);
    if (sid) {
        auth_session_destroy(sid);
    }

    auth_cleanup();

    /* Clear both cookies */
    printf("Status: 302\r\n");
    printf("Set-Cookie: %s=; Path=/; HttpOnly; Secure; SameSite=Lax; "
           "Max-Age=0\r\n", SESSION_COOKIE_NAME);
    printf("Set-Cookie: csrf_token=; Path=/; Secure; SameSite=Lax; "
           "Max-Age=0\r\n");
    printf("Location: /index.html\r\n\r\n");
    return 0;
}

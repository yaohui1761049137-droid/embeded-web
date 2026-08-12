/* main.cgi — Phase 2: auth gate using SQLite sessions
 * Validates session_id cookie, outputs control panel HTML template.
 */
#include "common.h"
#include "auth.h"

#define DB_PATH "/var/db/myapp.db"

int main(void) {
    char *sid = get_cookie(SESSION_COOKIE_NAME);

    if (!sid) {
        cgi_redirect("/index.html");
        return 0;
    }

    auth_init(DB_PATH);

    SessionInfo session;
    if (!auth_session_verify(sid, &session)) {
        auth_cleanup();
        cgi_redirect("/index.html");
        return 0;
    }

    /* ADR-0002: forced-change gate — expired password is redirected to
     * the change page instead of the control panel */
    if (auth_user_must_change_password(session.user_id)) {
        auth_cleanup();
        cgi_redirect("/change.html");
        return 0;
    }

    /* Days left until password expiry (ADR-0002 warning banner) */
    int days_left = auth_user_days_left(session.user_id);

    auth_cleanup();//断开数据库链接，省资源

    cgi_header("text/html; charset=utf-8");

    /* Inject current user info for frontend JS */
    printf("<script>window._currentUser={id:%d,username:\"%s\",role:\"%s\",csrf:\"%s\",daysLeft:%d};</script>\n",
           session.user_id, session.username, session.role, session.csrf_token,
           days_left);

    FILE *fp = fopen("/home/www/control_panel.html", "r");
    if (!fp) {
        printf("<html><body><h2>Error</h2><p>Page template not found</p></body></html>");
        return 0;
    }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        fwrite(buf, 1, n, stdout);
    fclose(fp);

    return 0;
}

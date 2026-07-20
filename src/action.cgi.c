/* action.cgi — Phase 2: SQLite session check, serial debug tool
 * Sends test data via serial port. Requires valid session_id cookie.
 */
#include "common.h"
#include "auth.h"

#define DB_PATH "/var/db/myapp.db"

int main(void) {
    const char *sid = get_cookie(SESSION_COOKIE_NAME);

    if (!sid) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Not authenticated\"}");
        return 0;
    }

    auth_init(DB_PATH);

    SessionInfo session;
    if (!auth_session_verify(sid, &session)) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Not authenticated\"}");
        auth_cleanup();
        return 0;
    }

    auth_cleanup();

    int fd = serial_open(SERIAL_DEVICE, CUSTOM_BAUD);

    cgi_header("application/json");
    if (fd < 0) {
        printf("{\"status\":\"error\",\"message\":\"Cannot open %s\"}", SERIAL_DEVICE);
        return 0;
    }

    const char *data = "Hello from Lighttpd Server!\r\n";
    int sent = serial_send(fd, data, strlen(data));
    serial_close(fd);

    if (sent < 0)
        printf("{\"status\":\"error\",\"message\":\"Serial write failed\"}");
    else
        printf("{\"status\":\"ok\",\"message\":\"%d bytes sent via %s at %d baud\"}",
               sent, SERIAL_DEVICE, CUSTOM_BAUD);

    return 0;
}

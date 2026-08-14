/* action.cgi — Phase 2: SQLite session check, serial debug tool
 * Sends test data via serial port. Requires valid session_id cookie.
 */
#include "common.h"
#include "auth.h"
#include "gate.h"

int main(void) {
    /* Request gate: emits error JSON on failure, cleans up itself */
    SessionInfo session;
    if (!gate_json_session(&session)) return 0;

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

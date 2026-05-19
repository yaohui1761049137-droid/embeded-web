/*
 * action.cgi - Handle serial port send action
 * Requires valid session cookie
 * Sends fixed test data via serial port at 150000 baud
 */
#include "common.h"

int main() {
    char *token = get_cookie("boa_token");

    if (!token || !session_check(token)) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Not authenticated\"}");
        return 0;
    }

    /* Open serial port */
    int fd = serial_open(SERIAL_DEVICE, CUSTOM_BAUD);

    cgi_header("application/json");

    if (fd < 0) {
        printf("{\"status\":\"error\",\"message\":\"Cannot open %s\"}", SERIAL_DEVICE);
        return 0;
    }

    /* Send fixed test data */
    const char *data = "Hello from LubanCat Boa Server!\r\n";
    int len = strlen(data);
    int sent = serial_send(fd, data, len);

    serial_close(fd);

    if (sent < 0) {
        printf("{\"status\":\"error\",\"message\":\"Serial write failed\"}");
    } else {
        printf("{\"status\":\"ok\",\"message\":\"%d bytes sent via %s at %d baud\"}", sent, SERIAL_DEVICE, CUSTOM_BAUD);
    }

    return 0;
}

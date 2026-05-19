/*
 * logout.cgi - Destroy session and redirect to login
 */
#include "common.h"

int main() {
    char *token = get_cookie("boa_token");
    if (token) {
        session_destroy(token);
    }

    /* Clear cookie and redirect */
    printf("Status: 302\r\n");
    printf("Set-Cookie: boa_token=; path=/; max-age=0\r\n");
    printf("Location: /index.html\r\n\r\n");
    return 0;
}

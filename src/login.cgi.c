/*
 * login.cgi - Handle user login
 * Expects POST with user=admin&pass=admin
 * On success: sets session cookie and redirects to main.cgi
 * On failure: shows error page
 */
#include "common.h"

int main() {
    const char *req_method = get_env("REQUEST_METHOD");

    if (strcmp(req_method, "POST") == 0) {
        char *user = get_post_param("user");
        char *pass = get_post_param("pass");

        if (user && pass &&
            strcmp(user, "admin") == 0 &&
            strcmp(pass, "admin") == 0) {

            char token[TOKEN_LEN] = {0};
            if (session_create("admin", token)) {
                printf("Status: 302\r\n");
                printf("Set-Cookie: boa_token=%s; path=/; max-age=3600\r\n", token);
                printf("Location: /cgi-bin/main.cgi\r\n\r\n");
                return 0;
            }
        }

        /* Login failed */
        cgi_header("text/html; charset=utf-8");
        printf("<html><head><meta charset='utf-8'>");
        printf("<meta http-equiv='refresh' content='2;url=/index.html'>");
        printf("<title>Login Failed</title></head><body>");
        printf("<h2>Login Failed</h2>");
        printf("<p>Invalid username or password. Redirecting...</p>");
        printf("</body></html>");
        return 0;
    }

    /* GET request - show login form */
    cgi_header("text/html; charset=utf-8");
    printf("<html><head><meta charset='utf-8'>");
    printf("<title>Login - LubanCat Boa</title>");
    printf("<link rel='stylesheet' href='/style.css'>");
    printf("</head><body>");
    printf("<div class='login-container'>");
    printf("<h1>LubanCat Web Control</h1>");
    printf("<form method='POST' action='/cgi-bin/login.cgi'>");
    printf("<label>Username:</label>");
    printf("<input type='text' name='user' value='admin'><br>");
    printf("<label>Password:</label>");
    printf("<input type='password' name='pass' value='admin'><br>");
    printf("<input type='submit' value='Login'>");
    printf("</form></div></body></html>");
    return 0;
}

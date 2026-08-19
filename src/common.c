#include "common.h"
/* === HTTP Helpers === */

void cgi_header(const char *content_type) {
    printf("Content-Type: %s\r\n\r\n", content_type);
}

void cgi_redirect(const char *url) {
    printf("Status: 302\r\nLocation: %s\r\n\r\n", url);
}

char *get_env(const char *key) {
    char *val = getenv(key);
    return val ? val : "";
}

/* Parse Cookie header to get a specific cookie value */
char *get_cookie(const char *name) {
    const char *cookie = getenv("HTTP_COOKIE");
    if (!cookie || !*cookie) return NULL;

    static char value[256];
    const char *p = cookie;
    int name_len = strlen(name);

    while (*p) {
        /* skip spaces */
        while (*p == ' ') p++;
        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            p += name_len + 1;
            int i = 0;
            while (*p && *p != ';' && *p != ' ' && i < 255) {
                value[i++] = *p++;
            }
            value[i] = '\0';
            return value;
        }
        /* skip to next cookie */
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
    }
    return NULL;
}

/* === POST Body Parsing === */
/* Cache the POST body on first read */
static char *cached_body = NULL;
static int cached_body_len = 0;

static void cache_post_body(void) {
    if (cached_body) return;
    const char *cl_str = getenv("CONTENT_LENGTH");
    if (!cl_str || !*cl_str) return;
    int cl = atoi(cl_str);
    if (cl <= 0 || cl > 65536) return;
    cached_body = malloc(cl + 1);
    if (!cached_body) return;
    int read_total = 0;
    while (read_total < cl) {
        int r = fread(cached_body + read_total, 1, cl - read_total, stdin);
        if (r <= 0) break;
        read_total += r;
    }
    cached_body[read_total] = '\0';
    cached_body_len = read_total;
}

/* Read POST body and extract param value.
 * Returns a freshly malloc'd URL-decoded copy, or NULL if the param is
 * absent. The copy is never freed — the CGI process exits at request
 * end. (The old rotating pool of 6 slots was removed: its slot count
 * bound correctness to how many times callers invoked us.) */
char *get_post_param(const char *param_name) {
    cache_post_body();
    if (!cached_body) return NULL;

    int name_len = strlen(param_name);
    char *p = cached_body;

    while (p && *p) {
        if (strncmp(p, param_name, name_len) == 0 && p[name_len] == '=') {
            p += name_len + 1;
            char *end = strchr(p, '&');
            int val_len = end ? (int)(end - p) : (int)strlen(p);

            /* URL decode into a fresh buffer sized to the value */
            char *result = malloc(val_len + 1);
            if (!result) return NULL;
            int i = 0, j = 0;
            while (j < val_len) {
                if (p[j] == '%' && j + 2 < val_len) {
                    char hex[3] = {p[j+1], p[j+2], '\0'};
                    result[i++] = (char)strtol(hex, NULL, 16);
                    j += 3;
                } else if (p[j] == '+') {
                    result[i++] = ' ';
                    j++;
                } else {
                    result[i++] = p[j++];
                }
            }
            result[i] = '\0';
            return result;
        }
        /* Skip to next param */
        p = strchr(p, '&');
        if (p) p++;
    }
    return NULL;
}

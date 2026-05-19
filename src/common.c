#include "common.h"
#include <sys/ioctl.h>
#include <asm-generic/termbits.h>
#include <dirent.h>
#include <stdarg.h>

/* === HTTP Helpers === */

void cgi_header(const char *content_type) {
    printf("Content-Type: %s\r\n\r\n", content_type);
}

void cgi_redirect(const char *url) {
    printf("Status: 302\r\nLocation: %s\r\n\r\n", url);
}

void cgi_error(const char *msg) {
    cgi_header("text/html; charset=utf-8");
    printf("<html><body><h2>Error</h2><p>%s</p></body></html>", msg);
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

/* Generate a simple token from username + time + pid */
static void generate_token(const char *username, char *out) {
    unsigned long h = 5381;
    const char *s = username;
    while (*s) h = ((h << 5) + h) + *s++;

    time_t t = time(NULL);
    h = ((h << 5) + h) + (unsigned long)t;
    h = ((h << 5) + h) + (unsigned long)getpid();

    snprintf(out, TOKEN_LEN, "%016lx%08x", h, rand() % 0x10000000);
}

/* Create a session file. Returns 1 on success, 0 on failure. */
int session_create(const char *username, char *token_out) {
    /* Ensure session directory exists (0777 so 'nobody' user can write) */
    mkdir(SESSION_DIR, 0777);
    chmod(SESSION_DIR, 0777);

    generate_token(username, token_out);

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", SESSION_DIR, token_out);

    FILE *fp = fopen(path, "w");
    if (!fp) return 0;

    fprintf(fp, "%s\n%ld\n", username, time(NULL));
    fclose(fp);
    return 1;
}

/* Check if a session token is valid. Returns 1 if valid, 0 if invalid. */
int session_check(const char *token) {
    if (!token || !*token) return 0;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", SESSION_DIR, token);

    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    char line[128];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }

    long created_time = 0;
    if (fgets(line, sizeof(line), fp)) {
        created_time = atol(line);
    }
    fclose(fp);

    if (created_time == 0) return 0;

    time_t now = time(NULL);
    if ((now - created_time) > SESSION_VALID_SECS) {
        unlink(path);
        return 0;
    }

    return 1;
}

void session_destroy(const char *token) {
    if (!token || !*token) return;
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", SESSION_DIR, token);
    unlink(path);
}

/* === Serial Port === */

int serial_open(const char *device, unsigned int baudrate) {
    int fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) return -1;

    /* Use termios2 for custom baud rate */
    struct termios2 tio;
    if (ioctl(fd, TCGETS2, &tio)) {
        close(fd);
        return -1;
    }

    tio.c_cflag &= ~CBAUD;
    tio.c_cflag |= BOTHER;
    tio.c_cflag |= CS8 | CLOCAL | CREAD;
    tio.c_cflag &= ~(CSTOPB | PARENB | PARODD | CSIZE);
    tio.c_cflag |= CS8;

    tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tio.c_oflag &= ~OPOST;
    tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 10;

    tio.c_ispeed = baudrate;
    tio.c_ospeed = baudrate;

    if (ioctl(fd, TCSETS2, &tio)) {
        close(fd);
        return -1;
    }

    /* Flush */
    ioctl(fd, TCFLSH, TCIOFLUSH);
    return fd;
}

int serial_send(int fd, const char *data, int len) {
    if (fd < 0 || !data) return -1;
    return write(fd, data, len);
}

void serial_close(int fd) {
    if (fd >= 0) close(fd);
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

/* Read POST body and extract param value */
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

            /* URL decode into result buffer */
            static char result[256];
            int i = 0, j = 0;
            while (j < val_len && i < 255) {
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

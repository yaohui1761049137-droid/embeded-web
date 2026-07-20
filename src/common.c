#include "common.h"
#include <sys/ioctl.h>
#include <sys/select.h>
#include <asm-generic/termbits.h>
#include <dirent.h>
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

/* Read one line (until \n) from serial fd with timeout_ms.
 * Returns bytes read, 0 on timeout, -1 on error. Strips \r. */
int serial_read_line(int fd, char *buf, int max_len, int timeout_ms) {
    fd_set set;
    struct timeval tv;
    int pos = 0;

    while (pos < max_len - 1) {
        FD_ZERO(&set);
        FD_SET(fd, &set);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int ret = select(fd + 1, &set, NULL, NULL, &tv);
        if (ret < 0) return -1;       /* select error */
        if (ret == 0) break;           /* timeout — return what we have */

        char c;
        ret = read(fd, &c, 1);
        if (ret <= 0) break;           /* read error or EOF */

        if (c == '\n') break;          /* end of line */
        if (c != '\r') buf[pos++] = c; /* strip CR */
    }

    buf[pos] = '\0';
    return pos;
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
 * Uses a rotating pool of buffers so multiple sequential calls don't
 * overwrite each other's results. */
#define PARAM_POOL 6
#define PARAM_SIZE 256
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

            /* URL decode into rotating result buffer pool */
            static char pool[PARAM_POOL][PARAM_SIZE];
            static int idx = 0;
            char *result = pool[idx];
            idx = (idx + 1) % PARAM_POOL;
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

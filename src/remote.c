/* remote.c — Remote Board protocol client (see remote.h for contract)
 *
 * Extracted from network.cgi.c: the command vocabulary, retry policy,
 * timeout, and OK/ERR grammar used to live in a static send_cmd there;
 * the board table was four #defines in common.h plus a port=s4 if/else
 * in the CGI.  All of it now lives here, so the protocol is testable
 * offline (pty fake board) and Board 3/4 is one table row.
 *
 * Error strings and wire behavior are verbatim from the original CGI.
 */
#include "remote.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

#define CMD_TIMEOUT_MS  2000   /* wait per response attempt */
#define MAX_RETRIES     3
#define RESP_BUF        256

/* Board table: port key ("" = default) → device/baud.
 * Board 3/4 = add a row here + an entry in the front-end registry
 * (www/control_panel.html BOARDS) — test_frontend.py checks both
 * tables stay in sync on the port key. */
typedef struct {
    const char *port;
    const char *device;
    unsigned int baud;
} BoardDef;

static const BoardDef g_boards[] = {
    { "",   "/dev/ttyS7", 115200 },   /* Board 1 (default) */
    { "s4", "/dev/ttyS4", 38400  },   /* Board 2 */
};
#define BOARD_COUNT ((int)(sizeof(g_boards) / sizeof(g_boards[0])))

struct RemoteBoard {
    int fd;
};

static void fail(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg);
}

int remote_board_lookup(const char *port) {
    if (!port) port = "";
    for (int i = 0; i < BOARD_COUNT; i++)
        if (strcmp(g_boards[i].port, port) == 0)
            return i;
    return -1;
}

/* Send one command with the retry policy + OK/ERR grammar.
 * Returns 1 = OK response, 0 = no useful response (err set),
 * -2 = ERR response (err set to the payload), -1 = send failure.
 * This is the original send_cmd from network.cgi.c, moved. */
static int send_cmd(int fd, const char *cmd, char *resp, int max_resp,
                    char *err, size_t errlen) {
    int cmd_len = strlen(cmd);
    if (cmd_len < 1 || cmd_len > 200) {
        fail(err, errlen, "Invalid command");
        return -1;
    }

    char buf[256];
    memcpy(buf, cmd, cmd_len);
    buf[cmd_len] = '\n';
    buf[cmd_len + 1] = '\0';

    int retry;
    for (retry = 0; retry < MAX_RETRIES; retry++) {
        tcflush(fd, TCIFLUSH);
        if (serial_send(fd, buf, cmd_len + 1) < 0) {
            fail(err, errlen, "Serial send failed");
            return -1;
        }
        int n = serial_read_line(fd, resp, max_resp, CMD_TIMEOUT_MS);
        if (n <= 0) {
            if (retry < MAX_RETRIES - 1) { usleep(100000); continue; }
            fail(err, errlen, "Remote Board not responding");
            return 0;
        }
        if (strncmp(resp, "OK ", 3) == 0 || strcmp(resp, "OK") == 0) return 1;
        if (strncmp(resp, "ERR", 3) == 0) {
            const char *p = resp + 3;
            while (*p == ' ') p++;
            fail(err, errlen, p);
            return -2;
        }
        if (retry < MAX_RETRIES - 1) usleep(100000);
    }
    char m[64];
    snprintf(m, sizeof(m), "Garbled response after %d retries", MAX_RETRIES);
    fail(err, errlen, m);
    return 0;
}

RemoteBoard *remote_board_open(int board) {
    if (board < 0 || board >= BOARD_COUNT) return NULL;

    /* Device path overridable for offline tests (cf. gate's DB_PATH). */
    const char *dev = g_boards[board].device;
    const char *ovr = getenv("REMOTE_SERIAL_DEVICE_OVERRIDE");
    if (ovr && *ovr) dev = ovr;

    int fd = serial_open(dev, g_boards[board].baud);
    if (fd < 0) return NULL;

    /* PING warmup, result discarded — original behavior: one PING
     * per request, its outcome ignored. */
    char resp[RESP_BUF];
    send_cmd(fd, "PING", resp, sizeof(resp), NULL, 0);

    RemoteBoard *h = malloc(sizeof(*h));
    if (!h) { serial_close(fd); return NULL; }
    h->fd = fd;
    return h;
}

int remote_query(RemoteBoard *h, RemoteNetConfig *out, char *err, size_t errlen) {
    if (!h || !out) return -1;
    memset(out, 0, sizeof(*out));

    char resp[RESP_BUF];
    int rc4 = send_cmd(h->fd, "REMOTE_GET_IPV4", resp, sizeof(resp), err, errlen);
    if (rc4 == 1)
        sscanf(resp, "OK %63s %63s %63s", out->ip, out->mask, out->gateway);
    /* keep going to IPv6 even if IPv4 failed (original flow ran both) */

    int rc6 = send_cmd(h->fd, "REMOTE_GET_IPV6", resp, sizeof(resp), err, errlen);
    if (rc6 == 1) sscanf(resp, "OK %127s", out->ipv6);

    return (rc4 == 1 && rc6 == 1) ? 0 : -1;
}

int remote_configure(RemoteBoard *h, const RemoteNetConfig *cfg,
                     char *err, size_t errlen) {
    if (!h || !cfg) return -1;

    char resp[RESP_BUF], err4[128] = "", err6[128] = "";
    char cmd[256];
    int rc4, rc6 = 1;   /* IPv6 not attempted = success */

    snprintf(cmd, sizeof(cmd), "REMOTE_SET_IPV4 %s %s %s",
             cfg->ip, cfg->mask, cfg->gateway);
    rc4 = send_cmd(h->fd, cmd, resp, sizeof(resp), err4, sizeof(err4));

    if (cfg->ipv6 && *cfg->ipv6) {
        snprintf(cmd, sizeof(cmd), "REMOTE_SET_IPV6 %s", cfg->ipv6);
        rc6 = send_cmd(h->fd, cmd, resp, sizeof(resp), err6, sizeof(err6));
    }

    if (rc4 >= 1 && rc6 >= 1) return 0;

    /* Compose the combined message the original CGI built
     * ("IPv4: <msg>" + "; IPv6: <msg>").  IPv6-only failures now get
     * a real message instead of the original empty string. */
    char combined[512];
    if (rc4 < 1)
        snprintf(combined, sizeof(combined), "IPv4: %s", err4);
    else
        combined[0] = '\0';
    if (rc6 < 1) {
        size_t cur = strlen(combined);
        snprintf(combined + cur, sizeof(combined) - cur, "%sIPv6: %s",
                 cur ? "; " : "", err6);
    }
    fail(err, errlen, combined);
    return -1;
}

void remote_board_close(RemoteBoard *h) {
    if (!h) return;
    serial_close(h->fd);
    free(h);
}

/* network.cgi — Phase 2: SQLite sessions + CSRF protection
 *
 * GET  ?action=get  →  query Remote Board IPv4 + IPv6 via serial
 * POST ?action=set  →  set IP config (requires csrf_token)
 *
 * Both return JSON. Requires valid session_id cookie.
 */
#include "common.h"
#include "auth.h"
#include <termios.h>
#include <sys/select.h>

#define DB_PATH         "/var/db/myapp.db"
#define CMD_TIMEOUT_MS  2000
#define RESP_BUF        256

/* ── Serial helpers ──────────────────────────────────────────────── */

static int send_cmd(int fd, const char *cmd, char *resp, int max_resp,
                    char *err_msg, int err_max, int local) {
    int cmd_len = strlen(cmd);
    if (cmd_len < 1 || cmd_len > 200) {
        if (err_msg) snprintf(err_msg, err_max, "Invalid command");
        return -1;
    }

    char buf[256];
    memcpy(buf, cmd, cmd_len);
    buf[cmd_len] = '\n';
    buf[cmd_len + 1] = '\0';

    int max_retries = 3;
    int retry;
    for (retry = 0; retry < max_retries; retry++) {
        tcflush(fd, TCIFLUSH);
        if (serial_send(fd, buf, cmd_len + 1) < 0) {
            if (err_msg) snprintf(err_msg, err_max, "Serial send failed");
            return -1;
        }
        int n = local ? fifo_read_line(fd, resp, max_resp, CMD_TIMEOUT_MS)
                      : serial_read_line(fd, resp, max_resp, CMD_TIMEOUT_MS);
        if (n <= 0) {
            if (retry < max_retries - 1) { usleep(100000); continue; }
            if (err_msg) snprintf(err_msg, err_max, "Remote Board not responding");
            return 0;
        }
        if (strncmp(resp, "OK ", 3) == 0 || strcmp(resp, "OK") == 0) return 1;
        if (strncmp(resp, "ERR", 3) == 0) {
            const char *p = resp + 3;
            while (*p == ' ') p++;
            if (err_msg) snprintf(err_msg, err_max, "%s", p);
            return -2;
        }
        if (retry < max_retries - 1) usleep(100000);
    }
    if (err_msg) snprintf(err_msg, err_max, "Garbled response after %d retries", max_retries);
    return 0;
}

static void json_escape(const char *src, char *dst, int max) {
    int i, j;
    for (i = 0, j = 0; src[i] && j < max - 4; i++) {
        unsigned char c = src[i];
        if (c == '"')       { dst[j++] = '\\'; dst[j++] = '"';  }
        else if (c == '\\') { dst[j++] = '\\'; dst[j++] = '\\'; }
        else if (c == '\n') { dst[j++] = '\\'; dst[j++] = 'n';  }
        else if (c == '\r') { dst[j++] = '\\'; dst[j++] = 'r';  }
        else if (c < 0x20)  { dst[j++] = ' ';                  }
        else                 { dst[j++] = c;                     }
    }
    dst[j] = '\0';
}

/* ── Actions ─────────────────────────────────────────────────────── */

/* ADR-0001: local=1 走本机 FIFO(handler-local.sh + nmcli),否则走串口 */
static int open_channel(int local, const char *serial_dev, unsigned int baud) {
    return local ? fifo_open(LOCAL_FIFO_PATH, LOCAL_RESP_FIFO)
                 : serial_open(serial_dev, baud);
}

static void close_channel(int local, int fd) {
    if (local) fifo_close(fd); else serial_close(fd);
}

static int handle_get(const char *serial_dev, unsigned int baud, int local) {
    char resp[RESP_BUF], err[128];
    char ip[64] = "", mask[64] = "", gateway[64] = "";
    char ipv6[128] = "";

    int fd = open_channel(local, serial_dev, baud);
    if (fd < 0) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"%s\"}",
               local ? "本地通道不可用(handler-local.sh 未运行?)"
                     : "Cannot open serial port");
        return 0;
    }
    send_cmd(fd, "PING", resp, sizeof(resp), NULL, 0, local);

    int r = send_cmd(fd, "REMOTE_GET_IPV4", resp, sizeof(resp), err, sizeof(err), local);
    if (r == 1) sscanf(resp, "OK %63s %63s %63s", ip, mask, gateway);

    r = send_cmd(fd, "REMOTE_GET_IPV6", resp, sizeof(resp), err, sizeof(err), local);
    if (r == 1) sscanf(resp, "OK %127s", ipv6);

    close_channel(local, fd);

    /* If no IPv4 data was retrieved, the remote board didn't respond —
     * report an error instead of returning empty data with "ok" status. */
    if (ip[0] == '\0') {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"%s\"}",
               local ? "本机无响应(handler-local.sh 异常?)"
                     : "Board 2 无响应(未连接或 handler.sh 未运行?)");
        return 0;
    }

    cgi_header("application/json");
    printf("{\"status\":\"ok\",\"ipv4\":{\"ip\":\"%s\",\"mask\":\"%s\",\"gateway\":\"%s\"},\"ipv6\":\"%s\"}",
           ip, mask, gateway, ipv6);
    return 0;
}

static int handle_set(const char *serial_dev, unsigned int baud, int local) {
    char *ip      = get_post_param("ip");
    char *mask    = get_post_param("mask");
    char *gateway = get_post_param("gateway");
    char *ipv6    = get_post_param("ipv6");

    if (!ip || !mask || !gateway || !*ip || !*mask || !*gateway) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Missing IPv4 parameters\"}");
        return 0;
    }

    int fd = open_channel(local, serial_dev, baud);
    if (fd < 0) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"%s\"}",
               local ? "本地通道不可用(handler-local.sh 未运行?)"
                     : "Cannot open serial port");
        return 0;
    }

    char resp[RESP_BUF], err[256], err_summary[512] = "";
    int all_ok = 1;

    send_cmd(fd, "PING", resp, sizeof(resp), NULL, 0, local);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "REMOTE_SET_IPV4 %s %s %s", ip, mask, gateway);
    int r = send_cmd(fd, cmd, resp, sizeof(resp), err, sizeof(err), local);
    if (r < 1) { all_ok = 0; snprintf(err_summary, sizeof(err_summary), "IPv4: %s", err); }

    if (ipv6 && *ipv6) {
        snprintf(cmd, sizeof(cmd), "REMOTE_SET_IPV6 %s", ipv6);
        r = send_cmd(fd, cmd, resp, sizeof(resp), err, sizeof(err), local);
        if (r < 1) {
            all_ok = 0;
            int cur = strlen(err_summary);
            if (cur > 0 && cur < (int)sizeof(err_summary) - 8)
                snprintf(err_summary + cur, sizeof(err_summary) - cur, "; IPv6: %s", err);
        }
    }

    close_channel(local, fd);

    cgi_header("application/json");
    if (all_ok) {
        printf("{\"status\":\"ok\",\"message\":\"保存成功\"}");
    } else {
        char esc[1024];
        json_escape(err_summary, esc, sizeof(esc));
        printf("{\"status\":\"error\",\"message\":\"%s\"}", esc);
    }
    return all_ok ? 1 : 0;
}

/* ADR-0001: 取消本机看门狗回滚(发送 REMOTE_CONFIRM_IPV4 给 handler-local.sh) */
static int handle_confirm(void) {
    int fd = fifo_open(LOCAL_FIFO_PATH, LOCAL_RESP_FIFO);
    if (fd < 0) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"本地通道不可用(handler-local.sh 未运行?)\"}");
        return 0;
    }
    char resp[RESP_BUF], err[128];
    int r = send_cmd(fd, "REMOTE_CONFIRM_IPV4", resp, sizeof(resp), err, sizeof(err), 1);
    fifo_close(fd);

    cgi_header("application/json");
    if (r >= 1)
        printf("{\"status\":\"ok\",\"message\":\"已确认生效\"}");
    else
        printf("{\"status\":\"error\",\"message\":\"确认失败: %s\"}", err[0] ? err : "通道无响应");
    return r >= 1 ? 1 : 0;
}

/* ── Entry point ─────────────────────────────────────────────────── */

int main(void) {
    auth_init(DB_PATH);

    /* Session check */
    const char *sid = get_cookie(SESSION_COOKIE_NAME);
    SessionInfo session;
    if (!sid || !auth_session_verify(sid, &session)) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Not authenticated\"}");
        auth_cleanup();
        return 0;
    }

    /* ADR-0002: forced-change gate — expired password blocks everything
     * except the change-password CGI */
    if (!auth_require_password_current(&session)) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"密码已过期,请先修改密码\"}");
        auth_cleanup();
        return 0;
    }

    /* Parse action & port */
    const char *qs = get_env("QUERY_STRING");
    char action[16] = "";
    if (strncmp(qs, "action=", 7) == 0) {
        int i;
        for (i = 0; i < 15 && qs[7+i] && qs[7+i] != '&'; i++)
            action[i] = qs[7+i];
        action[i] = '\0';
    }

    /* ADR-0001: 默认本地模式(本机即 Board 1,经 FIFO 交给 handler-local.sh);
     * port=s4 → 远端 Board 2(串口) */
    int local = 1;
    const char *serial_dev = REMOTE_SERIAL_DEVICE_2;
    unsigned int baud     = REMOTE_SERIAL_BAUD_2;
    {
        const char *p = strstr(qs, "port=");
        if (p) {
            char port[8] = "";
            int i;
            for (i = 0; i < 7 && p[5+i] && p[5+i] != '&'; i++)
                port[i] = p[5+i];
            port[i] = '\0';
            if (strcmp(port, "s4") == 0) {
                local = 0;
                serial_dev = REMOTE_SERIAL_DEVICE_2;
                baud       = REMOTE_SERIAL_BAUD_2;
            }
        }
    }

    if (strcmp(action, "get") == 0) {
        auth_cleanup();
        return handle_get(serial_dev, baud, local);
    }

    /* For "set": CSRF check FIRST (before POST params consumed by handle_set) */
    if (strcmp(action, "set") == 0) {
        const char *csrf = get_post_param("csrf_token");
        if (!csrf || !auth_csrf_verify(&session, csrf)) {
            cgi_header("application/json");
            printf("{\"status\":\"error\",\"message\":\"CSRF token invalid\"}");
            auth_cleanup();
            return 0;
        }
        int result = handle_set(serial_dev, baud, local);
        if (result == 1) {
            auth_audit_log(session.user_id, "network_set", session.user_id,
                           local ? "Local board network config modified via nmcli"
                                 : "Remote Board network config modified via serial",
                           getenv("REMOTE_ADDR"));
        }
        auth_cleanup();
        return result;
    }

    /* ADR-0001: 取消本机看门狗回滚(新 IP 登录成功后由前端调用) */
    if (strcmp(action, "confirm") == 0) {
        const char *csrf = get_post_param("csrf_token");
        if (!csrf || !auth_csrf_verify(&session, csrf)) {
            cgi_header("application/json");
            printf("{\"status\":\"error\",\"message\":\"CSRF token invalid\"}");
            auth_cleanup();
            return 0;
        }
        auth_cleanup();
        return handle_confirm();
    }

    cgi_header("application/json");
    printf("{\"status\":\"error\",\"message\":\"Invalid action\"}");
    auth_cleanup();
    return 0;
}

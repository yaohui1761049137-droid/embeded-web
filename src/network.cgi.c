/* network.cgi — Phase 2: SQLite sessions + CSRF protection
 *
 * GET  ?action=get  →  query Remote Board IPv4 + IPv6 via serial
 * POST ?action=set  →  set IP config (requires csrf_token)
 *
 * Both return JSON. Requires valid session_id cookie.
 * Since the remote module: board lookup, protocol grammar, retry and
 * PING warmup all live in remote.c — this file is a thin adapter.
 */
#include "common.h"
#include "auth.h"
#include "gate.h"
#include "remote.h"
#include <string.h>

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

static int handle_get(int board) {
    RemoteNetConfig cfg;
    char err[128];
    RemoteBoard *h = remote_board_open(board);
    if (!h) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Cannot open serial port\"}");
        return 0;
    }
    /* rc ignored on purpose: the wire contract is "ok with empty
     * fields" when the board is unresponsive (front-end parses
     * ipv4/ipv6 unconditionally). */
    remote_query(h, &cfg, err, sizeof(err));
    remote_board_close(h);

    cgi_header("application/json");
    printf("{\"status\":\"ok\",\"ipv4\":{\"ip\":\"%s\",\"mask\":\"%s\",\"gateway\":\"%s\"},\"ipv6\":\"%s\"}",
           cfg.ip, cfg.mask, cfg.gateway, cfg.ipv6);
    return 0;
}

static int handle_set(int board) {
    char *ip      = get_post_param("ip");
    char *mask    = get_post_param("mask");
    char *gateway = get_post_param("gateway");
    char *ipv6    = get_post_param("ipv6");

    if (!ip || !mask || !gateway || !*ip || !*mask || !*gateway) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Missing IPv4 parameters\"}");
        return 0;
    }

    RemoteBoard *h = remote_board_open(board);
    if (!h) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Cannot open serial port\"}");
        return 0;
    }

    RemoteNetConfig cfg;
    strncpy(cfg.ip, ip, sizeof(cfg.ip) - 1);
    strncpy(cfg.mask, mask, sizeof(cfg.mask) - 1);
    strncpy(cfg.gateway, gateway, sizeof(cfg.gateway) - 1);
    cfg.ipv6[0] = '\0';
    if (ipv6) strncpy(cfg.ipv6, ipv6, sizeof(cfg.ipv6) - 1);

    char err[256], esc[1024];
    int r = remote_configure(h, &cfg, err, sizeof(err));
    remote_board_close(h);

    cgi_header("application/json");
    if (r == 0) {
        printf("{\"status\":\"ok\",\"message\":\"保存成功\"}");
    } else {
        json_escape(err, esc, sizeof(esc));
        printf("{\"status\":\"error\",\"message\":\"%s\"}", esc);
    }
    return r == 0 ? 1 : 0;
}

/* ── Entry point ─────────────────────────────────────────────────── */

int main(void) {
    /* Request gate: cookie → session verify (emits error JSON on failure) */
    SessionInfo session;
    if (!gate_json_session(&session)) return 0;

    /* Parse action & port (query-string parsing is HTTP policy — stays here) */
    const char *qs = get_env("QUERY_STRING");
    char action[16] = "";
    if (strncmp(qs, "action=", 7) == 0) {
        int i;
        for (i = 0; i < 15 && qs[7+i] && qs[7+i] != '&'; i++)
            action[i] = qs[7+i];
        action[i] = '\0';
    }

    char port[8] = "";
    {
        const char *p = strstr(qs, "port=");
        if (p) {
            int i;
            for (i = 0; i < 7 && p[5+i] && p[5+i] != '&'; i++)
                port[i] = p[5+i];
            port[i] = '\0';
        }
    }

    int board = remote_board_lookup(port);
    if (board < 0) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"Invalid board\"}");
        auth_cleanup();
        return 0;
    }

    if (strcmp(action, "get") == 0) {
        auth_cleanup();
        return handle_get(board);
    }

    /* For "set": CSRF check before any side effect (serial writes) */
    if (strcmp(action, "set") == 0) {
        const char *csrf = get_post_param("csrf_token");
        if (!gate_require_csrf(&session, csrf)) {
            auth_cleanup();
            return 0;
        }
        int ok = handle_set(board);   /* 1 = success */
        if (ok == 1) {
            auth_audit_log(session.user_id, "network_set", session.user_id,
                           "Remote Board network config modified via serial",
                           getenv("REMOTE_ADDR"));
        }
        /* exit code is meaningless for a CGI (lighttpd ignores it); the
         * old 1-on-success broke set -e pipelines in host tests */
        auth_cleanup();
        return 0;
    }

    cgi_header("application/json");
    printf("{\"status\":\"error\",\"message\":\"Invalid action\"}");
    auth_cleanup();
    return 0;
}

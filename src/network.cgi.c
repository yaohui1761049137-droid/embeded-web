/* network.cgi — local NIC configuration via NetworkManager (ADR-0003)
 *
 * GET  ?action=get&port=ethX  →  effective config JSON from nmcli
 * POST ?action=set            →  validate → apply → (eth0: arm rollback)
 *
 * Both return JSON. Requires valid session_id cookie. All nmcli
 * invocation, profile lookup and parameter validation live in nmcli.c;
 * this file is a thin adapter plus the eth0 rollback orchestration.
 */
#include "common.h"
#include "auth.h"
#include "gate.h"
#include "nmcli.h"
#include <string.h>
#include <sys/wait.h>

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

/* ── Rollback (eth0 only, ADR-0003 D10) ───────────────────────────── */

static const char *rollback_file(void) {
    const char *p = getenv("ROLLBACK_FILE");
    return (p && *p) ? p : "/var/db/rollback.json";
}

/* Snapshot the pre-change config and arm the 180 s watchdog.  The file
 * is sourced by /usr/local/bin/rollback_watchdog.sh, so values must be
 * shell-safe — they already are, since every field passed validation
 * (IPv4/IPv6/DNS shapes contain no spaces or metacharacters). */
static int arm_rollback(const char *nic, const NicConfig *old_cfg,
                        const NicConfig *new_cfg, char *err, size_t errlen) {
    const char *path = rollback_file();
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(err, errlen, "无法写入回滚状态 %s", path);
        return -1;
    }
    char old_cidr[80], new_cidr[80];
    snprintf(old_cidr, sizeof(old_cidr), "%s/%d",
             old_cfg->ip, nmcli_mask_to_prefix(old_cfg->mask));
    snprintf(new_cidr, sizeof(new_cidr), "%s/%d",
             new_cfg->ip, nmcli_mask_to_prefix(new_cfg->mask));
    fprintf(f,
            "nic=%s\nts=%ld\nconfirmed=0\n"
            "old_cidr=%s\nold_ip=%s\nold_mask=%s\nold_gateway=%s\nold_dns=%s\nold_ipv6=%s\n"
            "new_cidr=%s\nnew_ip=%s\nnew_mask=%s\nnew_gateway=%s\nnew_dns=%s\nnew_ipv6=%s\n",
            nic, (long)time(NULL),
            old_cidr, old_cfg->ip, old_cfg->mask, old_cfg->gateway, old_cfg->dns, old_cfg->ipv6,
            new_cidr, new_cfg->ip, new_cfg->mask, new_cfg->gateway, new_cfg->dns, new_cfg->ipv6);
    fclose(f);

    /* Best-effort: if arming fails (e.g. host test), the boot-time
     * rollback-recover.service still covers the pending state. */
    char *argv[] = { "sudo", "-n", "systemd-run", "--on-active=180",
                     "/usr/local/bin/rollback_watchdog.sh", NULL };
    pid_t pid = fork();
    if (pid == 0) {
        execvp("sudo", argv);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
    return 0;
}

/* ── Actions ─────────────────────────────────────────────────────── */

static int handle_get(const char *nic) {
    NicConfig cfg;
    char err[256], esc[1024];
    if (nmcli_get_config(nic, &cfg, err, sizeof(err)) < 0) {
        cgi_header("application/json");
        json_escape(err, esc, sizeof(esc));
        printf("{\"status\":\"error\",\"message\":\"%s\"}", esc);
        return 0;
    }
    cgi_header("application/json");
    printf("{\"status\":\"ok\",\"ipv4\":{\"ip\":\"%s\",\"mask\":\"%s\",\"gateway\":\"%s\"},\"dns\":\"%s\",\"ipv6\":\"%s\"}",
           cfg.ip, cfg.mask, cfg.gateway, cfg.dns, cfg.ipv6);
    return 0;
}

static int handle_set(const char *nic) {
    char *ip      = get_post_param("ip");
    char *mask    = get_post_param("mask");
    char *gateway = get_post_param("gateway");
    char *dns     = get_post_param("dns");
    char *ipv6    = get_post_param("ipv6");

    if (!ip || !mask || !*ip || !*mask) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"缺少 IPv4 参数\"}");
        return 0;
    }

    NicConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.ip, sizeof(cfg.ip), "%s", ip);
    snprintf(cfg.mask, sizeof(cfg.mask), "%s", mask);
    if (gateway) snprintf(cfg.gateway, sizeof(cfg.gateway), "%s", gateway);
    if (dns)     snprintf(cfg.dns, sizeof(cfg.dns), "%s", dns);
    if (ipv6)    snprintf(cfg.ipv6, sizeof(cfg.ipv6), "%s", ipv6);

    char err[512], esc[1024];
    if (!nmcli_validate_params(&cfg, err, sizeof(err))) {
        cgi_header("application/json");
        json_escape(err, esc, sizeof(esc));
        printf("{\"status\":\"error\",\"message\":\"%s\"}", esc);
        return 0;
    }

    /* eth0 changes are guarded by the rollback watchdog: snapshot the
     * old config first so it can be restored if nobody logs in. */
    NicConfig old_cfg;
    int rollback = (strcmp(nic, "eth0") == 0);
    if (rollback) {
        memset(&old_cfg, 0, sizeof(old_cfg));
        if (nmcli_get_config(nic, &old_cfg, err, sizeof(err)) < 0) {
            cgi_header("application/json");
            printf("{\"status\":\"error\",\"message\":\"无法读取当前配置，已取消修改\"}");
            return 0;
        }
    }

    if (nmcli_set_config(nic, &cfg, err, sizeof(err)) < 0) {
        cgi_header("application/json");
        json_escape(err, esc, sizeof(esc));
        printf("{\"status\":\"error\",\"message\":\"%s\"}", esc);
        return 0;
    }

    if (rollback && arm_rollback(nic, &old_cfg, &cfg, err, sizeof(err)) < 0) {
        cgi_header("application/json");
        json_escape(err, esc, sizeof(esc));
        printf("{\"status\":\"error\",\"message\":\"%s\"}", esc);
        return 0;
    }

    cgi_header("application/json");
    printf("{\"status\":\"ok\",\"message\":\"保存成功\"}");
    return 1;
}

/* ── Entry point ─────────────────────────────────────────────────── */

int main(void) {
    /* Request gate: cookie → session verify (emits error JSON on failure) */
    SessionInfo session;
    if (!gate_json_session(&session)) return 0;

    /* Parse action & NIC (query-string parsing is HTTP policy — stays here) */
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

    if (!nmcli_nic_valid(port)) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"无效的网口\"}");
        auth_cleanup();
        return 0;
    }

    if (strcmp(action, "get") == 0) {
        auth_cleanup();
        return handle_get(port);
    }

    /* For "set": CSRF check before any side effect (nmcli writes) */
    if (strcmp(action, "set") == 0) {
        const char *csrf = get_post_param("csrf_token");
        if (!gate_require_csrf(&session, csrf)) {
            auth_cleanup();
            return 0;
        }
        int ok = handle_set(port);   /* 1 = success */
        if (ok == 1) {
            char detail[128];
            snprintf(detail, sizeof(detail), "%s 网口网络配置已修改%s", port,
                     strcmp(port, "eth0") == 0 ? "（3 分钟未登录将自动回滚）" : "");
            auth_audit_log(session.user_id, "network_set", session.user_id,
                           detail, getenv("REMOTE_ADDR"));
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

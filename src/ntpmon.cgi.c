/* ntpmon.cgi — NTP access control + traffic stats for the
 * 「服务监控及报警」tab.
 *
 * GET  ?action=stats                        (any session) → JSON
 * POST ?action=acl_op&op=add|remove&...     (root only, CSRF) → JSON
 *
 * ACL semantics (chrony 3.4): additions apply at run time via `chronyc
 * allow|deny` (instant, no restart) and persist to the managed file;
 * removals rewrite the file and restart chronyd — see ntpmon.h.
 */
#include "common.h"
#include "auth.h"
#include "gate.h"
#include "ntpmon.h"
#include <string.h>

#define NTPMON_JSON_MAX 262144

static void json_escape_into(const char *src, char *dst, int max) {
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

static int handle_stats(void) {
    static char out[NTPMON_JSON_MAX];
    if (ntpmon_stats_json(out, sizeof(out)) < 0) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"统计数据过大\"}");
        return 0;
    }
    cgi_header("application/json");
    printf("%s", out);
    return 0;
}

static int handle_acl_op(const SessionInfo *session) {
    char *op     = get_post_param("op");
    char *action = get_post_param("action");
    char *cidr   = get_post_param("cidr");

    cgi_header("application/json");
    if (!op || (!strcmp(op, "add") && !action) || !cidr || !*cidr) {
        printf("{\"status\":\"error\",\"message\":\"缺少参数\"}");
        return 0;
    }
    if (strcmp(op, "add") != 0 && strcmp(op, "remove") != 0) {
        printf("{\"status\":\"error\",\"message\":\"无效的操作\"}");
        return 0;
    }
    if (strcmp(op, "add") == 0 &&
        strcmp(action, "allow") != 0 && strcmp(action, "deny") != 0) {
        printf("{\"status\":\"error\",\"message\":\"动作必须是 allow 或 deny\"}");
        return 0;
    }
    /* remove may carry an action to disambiguate when both actions exist for
     * the same prefix; when present it must still be well-formed. */
    if (strcmp(op, "remove") == 0 && action && *action &&
        strcmp(action, "allow") != 0 && strcmp(action, "deny") != 0) {
        printf("{\"status\":\"error\",\"message\":\"动作必须是 allow 或 deny\"}");
        return 0;
    }
    if (!ntpmon_cidr_valid(cidr)) {
        printf("{\"status\":\"error\",\"message\":\"无效的 CIDR 格式\"}");
        return 0;
    }

    /* clear pre-checks against the managed file (helper re-validates) */
    NtpAclRule rules[NTPMON_MAX_RULES];
    int n = ntpmon_acl_load(rules, NTPMON_MAX_RULES);
    int idx = (n >= 0) ? ntpmon_acl_find(rules, n, action, cidr) : -1;
    if (strcmp(op, "add") == 0 && idx >= 0) {
        printf("{\"status\":\"error\",\"message\":\"规则已存在\"}");
        return 0;
    }
    if (strcmp(op, "remove") == 0 && idx < 0) {
        printf("{\"status\":\"error\",\"message\":\"规则不存在\"}");
        return 0;
    }

    /* A remove without an explicit action still resolves to the matched
     * rule's action, so the helper never has to guess between two lines. */
    if (strcmp(op, "remove") == 0 && (!action || !*action) && idx >= 0)
        action = rules[idx].action;

    char err[512], esc[1024];
    if (ntpmon_acl_apply(op, action, cidr, err, sizeof(err)) < 0) {
        json_escape_into(err, esc, sizeof(esc));
        printf("{\"status\":\"error\",\"message\":\"%s\"}", esc);
        return 0;
    }

    char detail[160];
    if (strcmp(op, "add") == 0)
        snprintf(detail, sizeof(detail), "新增 %s %s", action, cidr);
    else
        snprintf(detail, sizeof(detail), "删除 %s %s（chrony 已重启重载）",
                 (action && *action) ? action : "", cidr);
    auth_audit_log(session->user_id, "ntp_acl_change", session->user_id,
                   detail, getenv("REMOTE_ADDR"));

    printf("{\"status\":\"ok\",\"message\":\"%s\"}",
           strcmp(op, "add") == 0 ? "规则已添加并即时生效" : "规则已删除（chrony 已重启）");
    return 0;
}

int main(void) {
    SessionInfo session;
    if (!gate_json_session(&session)) return 0;

    const char *qs = get_env("QUERY_STRING");
    char action[16] = "";
    if (strncmp(qs, "action=", 7) == 0) {
        int i;
        for (i = 0; i < 15 && qs[7+i] && qs[7+i] != '&'; i++)
            action[i] = qs[7+i];
        action[i] = '\0';
    }

    if (strcmp(action, "stats") == 0) {
        auth_cleanup();
        return handle_stats();
    }

    if (strcmp(action, "acl_op") == 0) {
        if (!gate_require_role(&session, "root")) {
            auth_cleanup();
            return 0;
        }
        const char *csrf = get_post_param("csrf_token");
        if (!gate_require_csrf(&session, csrf)) {
            auth_cleanup();
            return 0;
        }
        int rc = handle_acl_op(&session);
        auth_cleanup();
        return rc;
    }

    cgi_header("application/json");
    printf("{\"status\":\"error\",\"message\":\"Invalid action\"}");
    auth_cleanup();
    return 0;
}

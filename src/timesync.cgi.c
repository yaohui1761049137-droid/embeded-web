/* timesync.cgi — PPS+TOD+chrony sync status and UT986 receiver mode
 *
 * GET  ?action=status   →  chronyc sources/tracking + /run/pps_tod
 *                          state files as one JSON document
 * POST ?action=setmode  →  CSRF → whitelist the mode → write the fixed
 *                          $CFGGNSS/$CFGSAVE payloads to ttyS7
 *
 * Both return JSON and require a valid session cookie.  Serial writes
 * are fire-and-forget on purpose: reading the reply would compete with
 * the pps_tod daemon's RX thread (and steal NMEA sentences); whatever
 * the receiver echoes back is consumed and discarded by the daemon.
 */
#include "common.h"
#include "auth.h"
#include "gate.h"
#include "timesync.h"
#include <string.h>

#define TS_JSON_MAX 16384

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

static int handle_status(void) {
    char out[TS_JSON_MAX], err[128];
    if (timesync_status_json(out, sizeof(out), err, sizeof(err)) < 0) {
        cgi_header("application/json");
        printf("{\"status\":\"error\",\"message\":\"状态数据过长\"}");
        return 0;
    }
    cgi_header("application/json");
    printf("%s", out);
    return 0;
}

static int handle_setmode(const char *mode, const SessionInfo *session) {
    TimesyncMode m = timesync_mode_parse(mode);
    cgi_header("application/json");
    if (m == TS_MODE_UNKNOWN) {
        printf("{\"status\":\"error\",\"message\":\"无效的接收机模式\"}");
        return 0;
    }
    char err[256], esc[512];
    if (timesync_set_mode(m, err, sizeof(err)) < 0) {
        json_escape(err, esc, sizeof(esc));
        printf("{\"status\":\"error\",\"message\":\"%s\"}", esc);
        return 0;
    }
    const char *label = timesync_mode_label(m);
    char detail[128];
    snprintf(detail, sizeof(detail), "接收机模式切换为 %s", label);
    auth_audit_log(session->user_id, "timesync_setmode", session->user_id,
                   detail, getenv("REMOTE_ADDR"));

    /* Persist the last-set mode for the UI (the channel is write-only,
     * so this file is the only way the page can show it).  Best-effort:
     * the receiver already got the command either way. */
    char serr[160];
    int saved = (timesync_mode_save(m, serr, sizeof(serr)) == 0);

    printf("{\"status\":\"ok\",\"mode\":\"%s\",\"saved\":%s,"
           "\"message\":\"命令已发送，生效以状态数据为准\"}",
           mode, saved ? "true" : "false");
    return 0;
}

/* ── Entry point ─────────────────────────────────────────────────── */

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

    if (strcmp(action, "status") == 0) {
        auth_cleanup();
        return handle_status();
    }

    if (strcmp(action, "setmode") == 0) {
        /* CSRF check before any serial write */
        const char *csrf = get_post_param("csrf_token");
        if (!gate_require_csrf(&session, csrf)) {
            auth_cleanup();
            return 0;
        }
        char *mode = get_post_param("mode");
        int ok = handle_setmode(mode ? mode : "", &session);
        (void)ok;
        auth_cleanup();
        return 0;
    }

    cgi_header("application/json");
    printf("{\"status\":\"error\",\"message\":\"Invalid action\"}");
    auth_cleanup();
    return 0;
}
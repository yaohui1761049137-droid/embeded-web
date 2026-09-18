/* log.cgi — read-only system log viewer for the「系统日志」tab.
 *
 * GET ?action=sources                          → available log sources
 * GET ?action=tail&id=<src>&lines=N&grep=<kw>   → last N lines of one source
 *
 * Security model
 *  - The path is derived from a fixed id→path table; the client never
 *    contributes any path component, so traversal (`../../etc/passwd`) is
 *    structurally impossible — an unknown id is simply rejected.
 *  - Read-only: no fork/exec, no shell, no writes. Only the tail window
 *    (256 KB) of the file is read, so a 300 MB log costs the same as a
 *    small one.
 *  - root-only (gate_require_role): logs carry client IPs and operational
 *    detail, a higher sensitivity class than the world-readable ACL table.
 *
 * Offline testing: LOGVIEW_ROOT overrides the log root (/var/log) so the
 * CGI can run against a scratch tree in test_gate.sh.
 */
#include "common.h"
#include "auth.h"
#include "gate.h"
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define LOG_ROOT_DEFAULT "/var/log"
#define TAIL_WINDOW      (256 * 1024)  /* bytes read from the end of a file */
#define MAX_LINES        500           /* hard cap on returned lines */
#define MAX_LINE_CHARS   1000          /* per-line truncation */
#define MAX_GREP_LEN     64

typedef struct {
    const char *id;
    const char *label;
    const char *rel;      /* path under the log root; dated ones carry %04d-%02d-%02d */
    int dated;            /* 1 = 当日文件，文件名按本地日期生成 */
} LogSource;

static const LogSource g_sources[] = {
    { "pps_tod",      "授时守护进程（当日）", "pps_tod/pps_tod_%04d-%02d-%02d.log", 1 },
    { "pps_tod_wd",   "授时看门狗",           "pps_tod/watchdog.log",               0 },
    { "lighttpd_err", "Web 服务器错误日志",   "lighttpd/error.log",                 0 },
    { "lighttpd_acc", "Web 服务器访问日志",   "lighttpd/access.log",                0 },
};
#define NSOURCES ((int)(sizeof(g_sources) / sizeof(g_sources[0])))

static const char *log_root(void) {
    const char *r = getenv("LOGVIEW_ROOT");
    return (r && *r) ? r : LOG_ROOT_DEFAULT;
}

static int find_source(const char *id) {
    int i;
    if (!id || !*id) return -1;
    for (i = 0; i < NSOURCES; i++)
        if (strcmp(g_sources[i].id, id) == 0) return i;
    return -1;
}

static void source_path(const LogSource *s, char *out, size_t outlen) {
    if (s->dated) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(out, outlen, "%s/%s", log_root(), s->rel);   /* rel has no verbs */
        /* build the dated filename in two steps so the format string stays literal */
        snprintf(out, outlen, "%s/pps_tod/pps_tod_%04d-%02d-%02d.log",
                 log_root(), tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    } else {
        snprintf(out, outlen, "%s/%s", log_root(), s->rel);
    }
}

static void json_str(const char *src, char *dst, int max) {
    int i, j;
    for (i = 0, j = 0; src[i] && j < max - 4; i++) {
        unsigned char c = src[i];
        if (c == '"')       { dst[j++] = '\\'; dst[j++] = '"';  }
        else if (c == '\\') { dst[j++] = '\\'; dst[j++] = '\\'; }
        else if (c == '\n') { dst[j++] = '\\'; dst[j++] = 'n';  }
        else if (c == '\r') { dst[j++] = '\\'; dst[j++] = 'r';  }
        else if (c == '\t') { dst[j++] = '\\'; dst[j++] = 't';  }
        else if (c < 0x20)  { dst[j++] = ' ';                    }
        else                 { dst[j++] = src[i];                }
    }
    dst[j] = '\0';
}

/* Query-string parameter, URL-decoded.  get_post_param() only covers the
 * POST body; this is the GET counterpart.  Returns a malloc'd copy or NULL. */
static char *get_qs_param(const char *name) {
    const char *qs = getenv("QUERY_STRING");
    int name_len;
    const char *p;
    if (!qs) return NULL;
    name_len = (int)strlen(name);

    for (p = qs; p && *p; ) {
        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            const char *v = p + name_len + 1;
            const char *end = strchr(v, '&');
            int vlen = end ? (int)(end - v) : (int)strlen(v);
            char *out = malloc(vlen + 1);
            int i = 0, j = 0;
            if (!out) return NULL;
            while (j < vlen) {
                if (v[j] == '%' && j + 2 < vlen) {
                    char hex[3] = { v[j + 1], v[j + 2], '\0' };
                    out[i++] = (char)strtol(hex, NULL, 16);
                    j += 3;
                } else if (v[j] == '+') {
                    out[i++] = ' ';
                    j++;
                } else {
                    out[i++] = v[j++];
                }
            }
            out[i] = '\0';
            return out;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return NULL;
}

static void err_json(const char *msg) {
    char esc[256];
    json_str(msg, esc, sizeof(esc));
    cgi_header("application/json");
    printf("{\"status\":\"error\",\"message\":\"%s\"}", esc);
}

/* ── action=sources ────────────────────────────────────────────────── */
static int handle_sources(void) {
    int i, first = 1;
    cgi_header("application/json");
    printf("{\"status\":\"ok\",\"root\":\"");
    {
        char esc[512];
        json_str(log_root(), esc, sizeof(esc));
        printf("%s", esc);
    }
    printf("\",\"sources\":[");
    for (i = 0; i < NSOURCES; i++) {
        char path[512], tmstr[32] = "";
        struct stat st;
        int ok;
        source_path(&g_sources[i], path, sizeof(path));
        ok = (stat(path, &st) == 0 && S_ISREG(st.st_mode));
        if (ok) {
            struct tm tm;
            localtime_r(&st.st_mtime, &tm);
            strftime(tmstr, sizeof(tmstr), "%Y-%m-%d %H:%M:%S", &tm);
        }
        {
            char esc[512];
            json_str(path, esc, sizeof(esc));
            printf("%s{\"id\":\"%s\",\"label\":\"%s\",\"path\":\"%s\","
                   "\"exists\":%s,\"size\":%lld,\"mtime\":\"%s\"}",
                   first ? "" : ",", g_sources[i].id, g_sources[i].label, esc,
                   ok ? "true" : "false",
                   ok ? (long long)st.st_size : 0LL, tmstr);
        }
        first = 0;
    }
    printf("]}");
    return 0;
}

/* ── action=tail ───────────────────────────────────────────────────── */
static int handle_tail(void) {
    char *id = get_qs_param("id");
    char *lines_s = get_qs_param("lines");
    char *grep = get_qs_param("grep");
    int si, want, fd, nread = 0, r;
    long long size = 0, start = 0;
    int windowed = 0;
    char path[512];
    struct stat st;

    static char buf[TAIL_WINDOW + 1];
    static const char *keep_ptr[MAX_LINES];
    static int keep_len[MAX_LINES];
    int nkeep = 0, ring = 0;
    long long matched = 0;

    si = find_source(id);
    if (si < 0) { err_json("未知的日志源"); return 0; }   /* whitelist gate */

    want = lines_s ? atoi(lines_s) : 200;
    if (want < 1) want = 1;
    if (want > MAX_LINES) want = MAX_LINES;
    if (grep && strlen(grep) > MAX_GREP_LEN) grep[MAX_GREP_LEN] = '\0';

    source_path(&g_sources[si], path, sizeof(path));
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        char esc[512];
        json_str(path, esc, sizeof(esc));
        cgi_header("application/json");
        printf("{\"status\":\"ok\",\"id\":\"%s\",\"path\":\"%s\",\"exists\":false,"
               "\"size\":0,\"windowed\":false,\"matched\":0,\"returned\":0,"
               "\"truncated\":false,\"lines\":[]}", g_sources[si].id, esc);
        return 0;
    }
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) size = (long long)st.st_size;
    if (size > TAIL_WINDOW) { start = size - TAIL_WINDOW; windowed = 1; }
    if (lseek(fd, start, SEEK_SET) < 0) start = 0;
    while (nread < TAIL_WINDOW &&
           (r = (int)read(fd, buf + nread, TAIL_WINDOW - nread)) > 0)
        nread += r;
    close(fd);
    buf[nread] = '\0';

    /* Split into lines; keep the last N matching ones in a ring.  A window
     * that starts mid-file begins with a partial line — drop it. */
    {
        char *p = buf;
        char *end = buf + nread;
        if (windowed) {
            char *nl = memchr(buf, '\n', nread);
            p = nl ? nl + 1 : end;
        }
        while (p < end) {
            char *nl = memchr(p, '\n', (size_t)(end - p));
            int len = nl ? (int)(nl - p) : (int)(end - p);
            if (nl) *nl = '\0';
            if (len > 0 && p[len - 1] == '\r') p[--len] = '\0';
            if (!grep || !*grep || strstr(p, grep)) {
                if (len > MAX_LINE_CHARS) len = MAX_LINE_CHARS;
                keep_ptr[ring] = p;
                keep_len[ring] = len;
                ring = (ring + 1) % want;      /* ring size == want, so the
                                                * oldest slot is overwritten
                                                * once `want` matches exist */
                if (nkeep < want) nkeep++;
                matched++;
            }
            p = nl ? nl + 1 : end;
        }
    }

    cgi_header("application/json");
    {
        char esc[512];
        json_str(path, esc, sizeof(esc));
        printf("{\"status\":\"ok\",\"id\":\"%s\",\"path\":\"%s\",\"exists\":true,"
               "\"size\":%lld,\"windowed\":%s,\"matched\":%lld,\"returned\":%d,"
               "\"truncated\":%s,\"lines\":[",
               g_sources[si].id, esc, size, windowed ? "true" : "false",
               matched, nkeep, (matched > nkeep) ? "true" : "false");
    }
    {
        int i, base = (nkeep == want) ? ring : 0;
        char esc[MAX_LINE_CHARS * 2 + 8];
        for (i = 0; i < nkeep; i++) {
            const char *lp = keep_ptr[(base + i) % want];
            int ll = keep_len[(base + i) % want];
            char tmp[MAX_LINE_CHARS + 1];
            memcpy(tmp, lp, (size_t)ll);
            tmp[ll] = '\0';
            json_str(tmp, esc, sizeof(esc));
            printf("%s\"%s\"", i ? "," : "", esc);
        }
    }
    printf("]}");
    return 0;
}

int main(void) {
    SessionInfo session;
    char *action;

    if (!gate_json_session(&session)) return 0;
    if (!gate_require_role(&session, "root")) { auth_cleanup(); return 0; }

    action = get_qs_param("action");
    if (!action) {
        err_json("缺少 action");
    } else if (strcmp(action, "sources") == 0) {
        handle_sources();
    } else if (strcmp(action, "tail") == 0) {
        handle_tail();
    } else {
        err_json("无效的 action");
    }

    auth_cleanup();
    return 0;
}

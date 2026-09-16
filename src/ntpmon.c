/* ntpmon.c — NTP access control + traffic stats for the web UI.
 * See ntpmon.h for the data sources and the chrony 3.4 constraints.
 */
#include "ntpmon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define ACL_FILE_DEFAULT "/etc/chrony/acl-web.conf"
#define CSV_FILE_DEFAULT "/var/db/ntp_stats.csv"
#define HELPER_DEFAULT   "/usr/local/bin/chrony_acl_apply.sh"

/* ── paths (env-overridable for host tests) ───────────────────────── */

static const char *acl_file(void) {
    const char *p = getenv("NTPMON_ACL_FILE");
    return (p && *p) ? p : ACL_FILE_DEFAULT;
}

static const char *csv_file(void) {
    const char *p = getenv("NTPMON_CSV");
    return (p && *p) ? p : CSV_FILE_DEFAULT;
}

/* ── CIDR validation (shape only; chronyc is authoritative) ───────── */

static int v4_shape(const char *s) {
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%3u.%3u.%3u.%3u%c", &a, &b, &c, &d, &tail) != 4) return 0;
    return a <= 255 && b <= 255 && c <= 255 && d <= 255;
}

static int v6_shape(const char *s) {
    int dcolon = 0, groups = 0;
    const char *p = s;
    if (!strchr(s, ':')) return 0;
    while (*p) {
        if (*p == ':') {
            if (p[1] == ':') {
                dcolon++;
                p += 2;
                if (*p == 0) break;
            } else {
                p++;
            }
            continue;
        }
        int n = 0;
        while (isxdigit((unsigned char)*p)) { p++; n++; }
        if (n == 0 || n > 4) return 0;
        groups++;
        if (*p == 0) break;
        if (*p != ':') return 0;
    }
    if (dcolon > 1) return 0;
    if (dcolon == 1) return groups <= 7;
    return groups == 8;
}

int ntpmon_cidr_valid(const char *cidr) {
    if (!cidr || !*cidr || strlen(cidr) >= 64) return 0;
    const char *slash = strchr(cidr, '/');
    char addr[64];
    size_t alen = slash ? (size_t)(slash - cidr) : strlen(cidr);
    if (alen == 0 || alen >= sizeof(addr)) return 0;
    memcpy(addr, cidr, alen);
    addr[alen] = '\0';
    int prefix = -1;
    if (slash) {
        const char *p = slash + 1;
        if (!isdigit((unsigned char)*p)) return 0;
        prefix = 0;
        for (; *p; p++) {
            if (!isdigit((unsigned char)*p)) return 0;
            prefix = prefix * 10 + (*p - '0');
            if (prefix > 128) return 0;
        }
    }
    if (strchr(addr, ':')) {
        if (prefix > 128) return 0;
        return v6_shape(addr);
    }
    if (prefix > 32) return 0;
    return v4_shape(addr);
}

/* ── managed rule file ────────────────────────────────────────────── */

int ntpmon_acl_load(NtpAclRule *out, int max) {
    FILE *f = fopen(acl_file(), "r");
    if (!f) return -1;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char act[8], cidr[64];
        if (sscanf(p, "%7s %63s", act, cidr) != 2) continue;
        if (strcmp(act, "allow") != 0 && strcmp(act, "deny") != 0) continue;
        if (n < max) {
            snprintf(out[n].action, sizeof(out[n].action), "%s", act);
            snprintf(out[n].cidr, sizeof(out[n].cidr), "%s", cidr);
            n++;
        }
    }
    fclose(f);
    return n;
}

int ntpmon_acl_find(const NtpAclRule *rules, int n, const char *cidr) {
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(rules[i].cidr, cidr) == 0) return i;
    return -1;
}

/* ── helper invocation (fork/execvp, no shell) ────────────────────── */

int ntpmon_acl_apply(const char *op, const char *action, const char *cidr,
                     char *err, size_t errlen) {
    const char *override = getenv("NTPMON_HELPER_OVERRIDE");
    char *argv[8];
    int idx = 0;
    if (!override || !*override) {
        argv[idx++] = (char *)"sudo";
        argv[idx++] = (char *)"-n";
        argv[idx++] = (char *)HELPER_DEFAULT;
    } else {
        argv[idx++] = (char *)override;
    }
    argv[idx++] = (char *)op;
    if (strcmp(op, "add") == 0) argv[idx++] = (char *)action;
    argv[idx++] = (char *)cidr;
    argv[idx] = NULL;

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        snprintf(err, errlen, "pipe 失败");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        snprintf(err, errlen, "fork 失败");
        return -1;
    }
    if (pid == 0) {
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[0]);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    char out[512];
    size_t pos = 0, n;
    while (pos + 1 < sizeof(out) &&
           (n = (size_t)read(pipefd[0], out + pos, sizeof(out) - pos - 1)) > 0)
        pos += n;
    out[pos] = '\0';
    char tmp[256];
    while (read(pipefd[0], tmp, sizeof(tmp)) > 0) { }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        /* helper prints a human-readable reason on stderr */
        char *nl = strchr(out, '\n');
        if (nl) *nl = '\0';
        snprintf(err, errlen, "%s", out[0] ? out : "规则应用失败（helper 无输出）");
        return -1;
    }
    return 0;
}

/* ── JSON helpers ─────────────────────────────────────────────────── */

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

static size_t jappend(char *out, size_t outlen, size_t pos,
                      const char *fmt, ...) {
    if (pos >= outlen) return pos;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + pos, outlen - pos, fmt, ap);
    va_end(ap);
    if (n < 0) return pos;
    if ((size_t)n >= outlen - pos) return outlen;
    return pos + (size_t)n;
}

/* ── stats ────────────────────────────────────────────────────────── */

/* Parse one CSV line; returns 1 on success. */
static int parse_row(char *line, NtpMonRow *r) {
    long long v[10];
    char *p = line, *end;
    int i;
    for (i = 0; i < 10; i++) {
        v[i] = strtoll(p, &end, 10);
        if (end == p) return 0;
        p = end;
        if (i < 9) {
            if (*p != ',') return 0;
            p++;
        }
    }
    r->t = v[0]; r->hits = v[1]; r->drops = v[2];
    r->cmd_hits = v[3]; r->cmd_drops = v[4]; r->log_drops = v[5];
    r->eth[0] = v[6]; r->eth[1] = v[7]; r->eth[2] = v[8]; r->eth[3] = v[9];
    return 1;
}

int ntpmon_stats_json(char *out, size_t outlen) {
    size_t pos = 0;
    char buf[512];

    /* ── ACL rules ── */
    NtpAclRule rules[NTPMON_MAX_RULES];
    int nrules = ntpmon_acl_load(rules, NTPMON_MAX_RULES);
    pos = jappend(out, outlen, pos, "{\"status\":\"ok\",\"acl\":{\"ok\":%s,\"rules\":[",
                  nrules >= 0 ? "true" : "false");
    if (nrules > 0) {
        int i;
        for (i = 0; i < nrules; i++) {
            char ec[96];
            json_escape(rules[i].cidr, ec, sizeof(ec));
            pos = jappend(out, outlen, pos, "%s{\"action\":\"%s\",\"cidr\":\"%s\"}",
                          i ? "," : "", rules[i].action, ec);
        }
    }
    pos = jappend(out, outlen, pos, "]");

    /* static display metadata: rule file mtime */
    {
        struct stat st;
        if (stat(acl_file(), &st) == 0) {
            char ts[32];
            struct tm tm;
            localtime_r(&st.st_mtime, &tm);
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
            pos = jappend(out, outlen, pos, ",\"file_mtime\":\"%s\"", ts);
        } else {
            pos = jappend(out, outlen, pos, ",\"file_mtime\":\"\"");
        }
    }
    pos = jappend(out, outlen, pos, "},");

    /* ── sample rows (ring, keep the last NTPMON_MAX_ROWS) ── */
    static NtpMonRow rows[NTPMON_MAX_ROWS];
    int nrows = 0, head = 0;
    FILE *f = fopen(csv_file(), "r");
    if (f) {
        while (fgets(buf, sizeof(buf), f)) {
            NtpMonRow r;
            if (!parse_row(buf, &r)) continue;
            rows[(head + nrows) % NTPMON_MAX_ROWS] = r;
            if (nrows < NTPMON_MAX_ROWS) nrows++;
            else head = (head + 1) % NTPMON_MAX_ROWS;
        }
        fclose(f);
    }
    long long now = (long long)time(NULL);
    long long csv_ts = nrows ? rows[(head + nrows - 1) % NTPMON_MAX_ROWS].t : 0;

    /* series arrays: t / m (per-minute requests, -1 = break) / c (cumulative) */
    pos = jappend(out, outlen, pos, "\"series\":{\"t\":[");
    {
        int i, first = 1;
        for (i = 0; i < nrows; i++) {
            NtpMonRow *r = &rows[(head + i) % NTPMON_MAX_ROWS];
            pos = jappend(out, outlen, pos, "%s%lld", first ? "" : ",", r->t);
            first = 0;
        }
    }
    pos = jappend(out, outlen, pos, "],\"m\":[");
    {
        int i, first = 1;
        long long prev = 0, prev_t = 0;
        for (i = 0; i < nrows; i++) {
            NtpMonRow *r = &rows[(head + i) % NTPMON_MAX_ROWS];
            long long m = -1;
            if (i > 0) {
                long long dt = r->t - prev_t;
                long long d = r->hits - prev;
                /* counter reset (chronyd restart) or a sampling gap → break */
                if (d >= 0 && dt > 0 && dt <= 180)
                    m = (d * 60) / dt;
            }
            pos = jappend(out, outlen, pos, "%s%lld", first ? "" : ",", m);
            first = 0;
            prev = r->hits;
            prev_t = r->t;
        }
    }
    pos = jappend(out, outlen, pos, "],\"c\":[");
    {
        int i, first = 1;
        for (i = 0; i < nrows; i++) {
            NtpMonRow *r = &rows[(head + i) % NTPMON_MAX_ROWS];
            pos = jappend(out, outlen, pos, "%s%lld", first ? "" : ",", r->hits);
            first = 0;
        }
    }
    pos = jappend(out, outlen, pos, "]},");

    /* ── per-interface bars: cumulative + last-hour delta ── */
    pos = jappend(out, outlen, pos, "\"eth\":{\"names\":[\"eth0\",\"eth1\",\"eth2\",\"eth3\"],\"total\":[");
    {
        int i, first = 1;
        for (i = 0; i < 4; i++) {
            long long v = nrows ? rows[(head + nrows - 1) % NTPMON_MAX_ROWS].eth[i] : 0;
            pos = jappend(out, outlen, pos, "%s%lld", first ? "" : ",", v);
            first = 0;
        }
    }
    pos = jappend(out, outlen, pos, "],\"last_hour\":[");
    {
        int i, first = 1;
        for (i = 0; i < 4; i++) {
            long long v = 0;
            if (nrows) {
                NtpMonRow *last = &rows[(head + nrows - 1) % NTPMON_MAX_ROWS];
                int j, base = 0;
                for (j = nrows - 1; j >= 0; j--) {
                    NtpMonRow *r = &rows[(head + j) % NTPMON_MAX_ROWS];
                    if (last->t - r->t > 3600) break;
                    base = j;
                }
                v = last->eth[i] - rows[(head + base) % NTPMON_MAX_ROWS].eth[i];
                if (v < 0) v = 0;    /* counters reset within the window */
            }
            pos = jappend(out, outlen, pos, "%s%lld", first ? "" : ",", v);
            first = 0;
        }
    }
    pos = jappend(out, outlen, pos, "]},");

    /* ── meta ── */
    pos = jappend(out, outlen, pos,
                  "\"meta\":{\"csv_ok\":%s,\"csv_ts\":%lld,\"csv_age_s\":%lld,"
                  "\"samples\":%d}}",
                  f ? "true" : "false", csv_ts,
                  csv_ts ? now - csv_ts : -1, nrows);

    if (pos >= outlen) return -1;
    return 0;
}

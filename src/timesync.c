/* timesync.c — PPS+TOD+chrony sync status and UT986 receiver mode control
 *
 * Status JSON = chronyc -c sources/tracking + the pps_tod daemon's
 * per-second state files.  Receiver-mode control writes fixed
 * $CFGGNSS/$CFGSAVE payloads to the TOD serial device: write-only,
 * never reads (a second reader would steal NMEA sentences from
 * pps_tod and trip the watchdog) and never touches termios
 * (device-shared state).
 *
 * Offline host tests override TIMESYNC_DEV (serial device),
 * TIMESYNC_STATUS_DIR (state file directory) and CHRONYC_OVERRIDE
 * (chronyc executable).
 */
#include "timesync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define TS_DEV_DEFAULT   "/dev/ttyS7"
#define TS_DIR_DEFAULT   "/run/pps_tod"
#define TS_MAX_SOURCES   8

/* ── Receiver modes (fixed whitelist, UT986 spec 1.4.2.7) ───────────
 * sysMask payloads carry precomputed checksums (XOR from '$' to '*',
 * both excluded); verified against the spec's rule. */
typedef struct {
    const char *name;
    const char *label;
    const char *cmd;    /* $CFGGNSS + $CFGSAVE (Bit4: CFGGNSS only) */
} ModeEntry;

static const ModeEntry g_modes[] = {
    { "gnss", "GNSS 全系统", "$CFGGNSS,h70717D*7D\r\n$CFGSAVE,h10*06\r\n" },
    { "gps",  "GPS",         "$CFGGNSS,h0D*7B\r\n$CFGSAVE,h10*06\r\n" },
    { "bds",  "北斗",         "$CFGGNSS,h70*08\r\n$CFGSAVE,h10*06\r\n" },
    { "gal",  "Galileo",     "$CFGGNSS,h7000*08\r\n$CFGSAVE,h10*06\r\n" },
    { "glo",  "GLONASS",     "$CFGGNSS,h100*3E\r\n$CFGSAVE,h10*06\r\n" },
};
#define TS_NMODES ((int)(sizeof(g_modes) / sizeof(g_modes[0])))

TimesyncMode timesync_mode_parse(const char *name) {
    int i;
    for (i = 0; i < TS_NMODES; i++)
        if (strcmp(g_modes[i].name, name) == 0) return (TimesyncMode)(i + 1);
    return TS_MODE_UNKNOWN;
}

const char *timesync_mode_label(TimesyncMode m) {
    if (m <= TS_MODE_UNKNOWN || m > TS_MODE_GLO) return NULL;
    return g_modes[m - 1].label;
}

/* ── Serial write (write-only, no termios) ────────────────────────── */

static const char *ts_dev_path(void) {
    const char *d = getenv("TIMESYNC_DEV");
    return (d && *d) ? d : TS_DEV_DEFAULT;
}

int timesync_set_mode(TimesyncMode m, char *err, size_t errlen) {
    if (m <= TS_MODE_UNKNOWN || m > TS_MODE_GLO) {
        snprintf(err, errlen, "无效的接收机模式");
        return -1;
    }
    const char *dev = ts_dev_path();
    int fd = open(dev, O_WRONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        snprintf(err, errlen, "无法打开串口 %s: %s", dev, strerror(errno));
        return -1;
    }
    const char *cmd = g_modes[m - 1].cmd;
    size_t len = strlen(cmd);
    ssize_t n = write(fd, cmd, len);
    int saved = errno;
    close(fd);
    if (n != (ssize_t)len) {
        snprintf(err, errlen, "写入串口 %s 失败: %s", dev, strerror(saved));
        return -1;
    }
    return 0;
}

/* ── Command execution (no shell), CHRONYC_OVERRIDE for tests ─────── */

static int ts_exec(char *const argv[], char *out, size_t outlen,
                   char *err, size_t errlen) {
    const char *exe = getenv("CHRONYC_OVERRIDE");
    if (!exe || !*exe) exe = argv[0];

    char *exec_argv[16];
    int idx = 0;
    exec_argv[idx++] = (char *)exe;
    int i;
    for (i = 1; argv[i] && idx < 15; i++) exec_argv[idx++] = argv[i];
    exec_argv[idx] = NULL;

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        snprintf(err, errlen, "pipe failed");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        snprintf(err, errlen, "fork failed");
        return -1;
    }
    if (pid == 0) {
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[0]);
        close(pipefd[1]);
        execvp(exec_argv[0], exec_argv);
        _exit(127);
    }
    close(pipefd[1]);

    size_t pos = 0;
    ssize_t n;
    char tmp[512];
    while (out && pos + 1 < outlen &&
           (n = read(pipefd[0], out + pos, outlen - pos - 1)) > 0)
        pos += (size_t)n;
    if (out && outlen) out[pos] = '\0';
    while (read(pipefd[0], tmp, sizeof(tmp)) > 0) { }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(err, errlen, "%s 失败: %s", argv[0],
                 (out && outlen) ? out : "no output");
        return -1;
    }
    return 0;
}

/* ── CSV handling for chronyc -c ──────────────────────────────────── */

/* chronyc -c hex-encodes strings that may contain commas/spaces (e.g.
 * the sources "last sample" column); decode those back, pass anything
 * else through unchanged.  All-hex guard (≥16 chars, even length)
 * keeps short numeric fields untouched. */
static void decode_field(const char *in, char *out, size_t outlen) {
    size_t l = strlen(in);
    if (l >= 16 && (l % 2) == 0) {
        size_t i;
        int allhex = 1;
        for (i = 0; i < l; i++)
            if (!isxdigit((unsigned char)in[i])) { allhex = 0; break; }
        if (allhex) {
            size_t j = 0;
            for (i = 0; i + 1 < l && j + 1 < outlen; i += 2) {
                unsigned int c;
                if (sscanf(in + i, "%2x", &c) != 1) break;
                out[j++] = (char)c;
            }
            out[j] = '\0';
            return;
        }
    }
    snprintf(out, outlen, "%s", in);
}

/* Split a line into comma-separated fields (in-place; the line buffer
 * is destroyed).  Returns the field count, ≤ nf. */
static int split_csv(char *line, char **fields, int nf) {
    int n = 0;
    fields[n++] = line;
    char *p = line;
    while (*p && n < nf) {
        if (*p == ',') {
            *p = '\0';
            fields[n++] = p + 1;
        }
        p++;
    }
    return n;
}

/* ── JSON building ────────────────────────────────────────────────── */

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
    if ((size_t)n >= outlen - pos) return outlen;   /* truncated */
    return pos + (size_t)n;
}

/* Emit one JSON object per -c sources row.  Two CSV layouts exist in
 * the wild and both are handled row-by-row:
 *   chrony 3.x (Debian Buster): #,*,PPS,0,2,377,3,-0.000018280,...
 *       flags and state are separate single-char columns; the last
 *       sample is decimal (column 7).
 *   chrony 4.x:                  #*,PPS,0,2,377,1,<hex-encoded>...
 *       MS merged in one column; last sample hex-encoded. */
static void emit_sources(const char *csv, char *out, size_t outlen, size_t *pos) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", csv);
    char *line = buf;
    int rows = 0;
    while (*line && rows < TS_MAX_SOURCES) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (*line) {
            char *f[8];
            int n = split_csv(line, f, 8);
            char ms[64], name[128], stratum[32], poll[32], reach[32],
                 last_rx[32], last_sample[128];
            if (n > 1 && strlen(f[1]) == 1) {
                /* chrony 3.x: f[0]=flags, f[1]=state, f[2]=name */
                snprintf(ms, sizeof(ms), "%s%s", f[0], f[1]);
                snprintf(name, sizeof(name), "%s", n > 2 ? f[2] : "");
                snprintf(stratum, sizeof(stratum), "%s", n > 3 ? f[3] : "");
                snprintf(poll, sizeof(poll), "%s", n > 4 ? f[4] : "");
                snprintf(reach, sizeof(reach), "%s", n > 5 ? f[5] : "");
                snprintf(last_rx, sizeof(last_rx), "%s", n > 6 ? f[6] : "");
                decode_field(n > 7 ? f[7] : "", last_sample, sizeof(last_sample));
            } else {
                /* chrony 4.x: f[0]=MS, f[1]=name */
                snprintf(ms, sizeof(ms), "%s", n > 0 ? f[0] : "");
                snprintf(name, sizeof(name), "%s", n > 1 ? f[1] : "");
                snprintf(stratum, sizeof(stratum), "%s", n > 2 ? f[2] : "");
                snprintf(poll, sizeof(poll), "%s", n > 3 ? f[3] : "");
                snprintf(reach, sizeof(reach), "%s", n > 4 ? f[4] : "");
                snprintf(last_rx, sizeof(last_rx), "%s", n > 5 ? f[5] : "");
                decode_field(n > 6 ? f[6] : "", last_sample, sizeof(last_sample));
            }
            char e1[512], e2[256], e3[64], e4[64], e5[64], e6[64], e7[128];
            json_escape(name, e1, sizeof(e1));
            json_escape(ms, e2, sizeof(e2));
            json_escape(stratum, e3, sizeof(e3));
            json_escape(poll, e4, sizeof(e4));
            json_escape(reach, e5, sizeof(e5));
            json_escape(last_rx, e6, sizeof(e6));
            json_escape(last_sample, e7, sizeof(e7));
            *pos = jappend(out, outlen, *pos,
                "%s{\"ms\":\"%s\",\"name\":\"%s\",\"stratum\":\"%s\","
                "\"poll\":\"%s\",\"reach\":\"%s\",\"last_rx\":\"%s\","
                "\"last_sample\":\"%s\"}",
                rows ? "," : "", e2, e1, e3, e4, e5, e6, e7);
            rows++;
        }
        if (!nl) break;
        line = nl + 1;
        while (*line == '\n') line++;   /* skip a lone \r's \n sibling */
    }
}

/* Emit one JSON object for -c tracking.  chrony 3.x prints 14 columns
 * (extra "Reference Name" after the ID, epoch-seconds ref time);
 * chrony 4.x prints 13 (ISO ref time, no name column).  The key order
 * is selected by column count; indices shift by one between layouts. */
static void emit_tracking(const char *csv, char *out, size_t outlen, size_t *pos) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", csv);
    size_t bl = strlen(buf);
    while (bl && (buf[bl-1] == '\n' || buf[bl-1] == '\r')) buf[--bl] = '\0';
    char *f[14];
    int n = split_csv(buf, f, 14);
    static const char *keys3[14] = {
        "ref_id", "ref_name", "stratum", "ref_time", "system_time",
        "last_offset", "rms_offset", "frequency", "residual_freq", "skew",
        "root_delay", "root_dispersion", "update_interval", "leap_status"
    };
    static const char *keys4[13] = {
        "ref_id", "stratum", "ref_time", "system_time", "last_offset",
        "rms_offset", "frequency", "residual_freq", "skew", "root_delay",
        "root_dispersion", "update_interval", "leap_status"
    };
    const char **keys = (n >= 14) ? keys3 : (const char **)keys4;
    int nk = (n >= 14) ? 14 : 13;
    char dec[14][128];
    char esc[14][256];
    int i;
    for (i = 0; i < nk; i++) {
        decode_field(i < n ? f[i] : "", dec[i], sizeof(dec[i]));
        json_escape(dec[i], esc[i], sizeof(esc[i]));
    }
    *pos = jappend(out, outlen, *pos, "{");
    int first = 1;
    for (i = 0; i < nk; i++) {
        *pos = jappend(out, outlen, *pos, "%s\"%s\":\"%s\"",
                       first ? "" : ",", keys[i], esc[i]);
        first = 0;
    }
    *pos = jappend(out, outlen, *pos, "}");
}

/* ── /run/pps_tod state files (key=value lines) ───────────────────── */

static const char *ts_dir(void) {
    const char *d = getenv("TIMESYNC_STATUS_DIR");
    return (d && *d) ? d : TS_DIR_DEFAULT;
}

/* Emit `,"k":"v"` for every key=value line with a safe key.  The
 * caller has already opened the JSON object with other members, so the
 * first pair carries a leading comma too.  Returns 1 if the file was
 * read, 0 if not. */
static int emit_kv_file(const char *path, char *out, size_t outlen, size_t *pos) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        size_t kl = strlen(key), vl = strlen(val);
        while (vl && (val[vl-1] == '\n' || val[vl-1] == '\r')) val[--vl] = '\0';
        if (kl == 0) continue;
        size_t i;
        int safe = 1;
        for (i = 0; i < kl; i++)
            if (!isalnum((unsigned char)key[i]) && key[i] != '_') { safe = 0; break; }
        if (!safe) continue;
        char esc[256];
        json_escape(val, esc, sizeof(esc));
        *pos = jappend(out, outlen, *pos, ",\"%s\":\"%s\"", key, esc);
    }
    fclose(f);
    return 1;
}

/* File freshness (age in seconds), -1 if unreadable. */
static long ts_file_age(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)(time(NULL) - st.st_mtime);
}

int timesync_status_json(char *out, size_t outlen, char *err, size_t errlen) {
    (void)err; (void)errlen;
    size_t pos = 0;
    char buf[4096];
    char cerr[256];
    char now[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(now, sizeof(now), "%Y-%m-%d %H:%M:%S", &tm);

    pos = jappend(out, outlen, pos, "{\"status\":\"ok\",\"ts\":\"%s\",", now);

    /* chrony */
    pos = jappend(out, outlen, pos, "\"chrony\":{\"ok\":");
    if (ts_exec((char *[]){"chronyc", "-c", "sources", NULL},
                buf, sizeof(buf), cerr, sizeof(cerr)) == 0) {
        pos = jappend(out, outlen, pos, "true,\"sources\":[");
        emit_sources(buf, out, outlen, &pos);
        pos = jappend(out, outlen, pos, "]");
    } else {
        char e[300];
        json_escape(cerr, e, sizeof(e));
        pos = jappend(out, outlen, pos, "false,\"error\":\"%s\"", e);
    }
    pos = jappend(out, outlen, pos, ",\"tracking\":");
    if (ts_exec((char *[]){"chronyc", "-c", "tracking", NULL},
                buf, sizeof(buf), cerr, sizeof(cerr)) == 0) {
        emit_tracking(buf, out, outlen, &pos);
    } else {
        char e[300];
        json_escape(cerr, e, sizeof(e));
        pos = jappend(out, outlen, pos, "{\"error\":\"%s\"}", e);
    }
    pos = jappend(out, outlen, pos, "},");

    /* pps_tod status file */
    char p1[256];
    snprintf(p1, sizeof(p1), "%s/status", ts_dir());
    long age = ts_file_age(p1);
    pos = jappend(out, outlen, pos, "\"pps_tod\":{\"ok\":%s,\"age_s\":%ld",
                  age >= 0 ? "true" : "false", age);
    if (age >= 0)
        emit_kv_file(p1, out, outlen, &pos);
    pos = jappend(out, outlen, pos, "},");

    /* watchdog state file */
    char p2[256];
    snprintf(p2, sizeof(p2), "%s/watchdog.state", ts_dir());
    age = ts_file_age(p2);
    pos = jappend(out, outlen, pos, "\"watchdog\":{\"ok\":%s,\"age_s\":%ld",
                  age >= 0 ? "true" : "false", age);
    if (age >= 0)
        emit_kv_file(p2, out, outlen, &pos);
    pos = jappend(out, outlen, pos, "}}");

    if (pos >= outlen) return -1;
    return 0;
}
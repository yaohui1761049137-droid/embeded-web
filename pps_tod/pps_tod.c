/*
 * pps_tod.c - PPS (GPIO edge) + TOD (NMEA serial) -> chrony SHM refclock
 * LubanCat 2N (RK3568, kernel 4.19, no pps-gpio driver)
 *
 * Pairing algorithm (integer edge counting):
 *  - NMEA sentence carries exact UTC second S and arrives within second S
 *    (right after the edge that started second S).
 *  - On sentence arrival the pending edge just before it is edge_S
 *    (sample assigned S); every edge after that increments a counter.
 *    The integer counter never drifts; new sentences re-anchor every second.
 *  - Samples go to chrony via SysV shared memory (key 0x4e545030+unit),
 *    shmTime struct, mode 1 (gpsd-compatible count/valid scheme).
 *  - Coarse fallback: when the anchor stream looks sane (anchor_seq) but the
 *    clock is more than 1.0s off, or the sample gate has swallowed >= -M
 *    consecutive samples, step CLOCK_REALTIME by the measured offset.
 *    The step preserves sub-second phase (an older revision wrote nsec=0,
 *    which discarded up to 1s and could land the clock in the gated band).
 *    Stepping is throttled to one per 10s and capped at -X per hour.
 *
 * usage: pps_tod [-g chip:line] [-e rising|falling|both] [-t /dev/ttyS3]
 *                [-b 9600] [-u unit] [-l logfile] [-s statfile] [-L sec]
 *                [-R MB] [-M gated_run] [-X steps_per_hour] [-A ntp] [-C] [-D] [-S]
 *   -L periodic STAT summary interval seconds (default 60; 0 = raw
 *      per-sample PPS/TOD lines, the pre-summary logging mode)
 *   -R total log size cap in MB (default 100; days are sealed+gzipped at
 *      rollover and kept indefinitely — only when the cap is exceeded are
 *      the oldest days deleted; 0 = keep everything)
 *   -M consecutive gated samples before a forced clock step (default 10).
 *      Closes the 0.5..1.0s band where the sample gate swallowed everything
 *      and the >1.0s coarse path never fired.
 *   -X coarse clock steps allowed per hour (default 6); beyond that the
 *      daemon stops writing the clock and reports reset_state=CAPPED, so a
 *      faulty reference cannot drag the clock in a loop.
 *   -C also disables all clock writes (sample gating stays active).
 *   -C disable coarse fallback, -D stay in foreground, -S disable status
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <linux/gpio.h>

#define SHMKEY_BASE 0x4e545030 /* "NTP0" */

typedef struct {
    int mode;
    volatile int count;
    time_t clockTimeStampSec; int clockTimeStampUSec;
    time_t receiveTimeStampSec; int receiveTimeStampUSec;
    int leap; int precision; int nsamples;
    volatile int valid;
    int clockTimeStampNSec; int receiveTimeStampNSec;
    int dummy[8];
} shmTime_t;

static shmTime_t *shm = NULL;
static int shm_unit = 0;
static long long last_sample_mono = 0;
static long long last_nudge_mono = 0;
static int coarse_enabled = 1;
static long long last_off_mono = 0;   /* when last_off_us was refreshed */

/* per-second status file: consumed by the external watchdog */
static char status_path[128] = "/run/pps_tod/status";
static int status_enabled = 1;
static long long last_status_mono = 0;
static long long last_off_us = 0;     /* last sample offset (us), good or gated */
static int sec_gated = 0, sec_noedge = 0, sec_noanchor = 0, sec_badtod = 0;
static long long prev_rt_ns = 0, prev_mraw_ns = 0;  /* clock-consistency diag bases */

static char logdir[128] = "/var/log/pps_tod";
static char logfile[128] = "/var/log/pps_tod/pps_tod_YYYY-MM-DD.log";
static FILE *logfp = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static int log_day = 0;     /* day (yyyymmdd) of the currently open file */
static int log_roll = 1;    /* daily rollover; disabled when -l is given */
static int log_interval = 60;  /* -L: seconds per periodic STAT summary; 0 = raw per-sample lines */
static int retain_mb = 100;    /* -R: total log size cap in MB (start + daily check); 0 = keep all */

/* periodic log accumulators: written by the serial and gpio threads,
   snapshot+reset by emit_summary() in the gpio thread */
static pthread_mutex_t acc_mutex = PTHREAD_MUTEX_INITIALIZER;
static int acc_n = 0;                 /* accepted PPS samples in the window */
static long long acc_off_min_us = 0, acc_off_max_us = 0, acc_off_sum_us = 0;
static int acc_tod_n = 0;             /* TOD anchors in the window */
static int acc_gated = 0, acc_noedge = 0, acc_noanchor = 0, acc_badtod = 0;
static long long last_summary_mono = 0;
static int last_prune_day = 0;        /* yyyymmdd of the last retention run */

/* open the day log; call with log_mutex held (or before threads start) */
static void log_open(void) {
    static int dir_done = 0;
    struct tm *tm;
    time_t now = time(NULL);
    if (!dir_done) {
        mkdir(logdir, 0755);
        dir_done = 1;
    }
    tm = localtime(&now);
    int newday = (tm->tm_year + 1900) * 10000 + (tm->tm_mon + 1) * 100 + tm->tm_mday;
    if (log_roll && logfp && newday != log_day) {
        /* rolling to a new day: gzip the day file we are leaving */
        char oldpath[160], cmd[192];
        snprintf(oldpath, sizeof(oldpath), "%s/pps_tod_%04d-%02d-%02d.log",
                 logdir, log_day / 10000, (log_day / 100) % 100, log_day % 100);
        fclose(logfp);
        logfp = NULL;
        snprintf(cmd, sizeof(cmd), "gzip -f %s", oldpath);
        (void)system(cmd);
    }
    log_day = newday;
    if (log_roll)
        snprintf(logfile, sizeof(logfile), "%s/pps_tod_%04d-%02d-%02d.log",
                 logdir, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    if (logfp) fclose(logfp);
    logfp = fopen(logfile, "a");
}

static void logmsg(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    /* Event lines carry an HH:MM:SS prefix so incidents can be pinned to
       a wall-clock time; STAT summaries already embed a full ts= field
       (and may be emitted before the clock is stepped). */
    char line[544];
    if (strncmp(buf, "STAT:", 5) == 0) {
        snprintf(line, sizeof(line), "%s", buf);
    } else {
        struct tm tm;
        time_t now = time(NULL);
        localtime_r(&now, &tm);
        snprintf(line, sizeof(line), "%02d:%02d:%02d %s",
                 tm.tm_hour, tm.tm_min, tm.tm_sec, buf);
    }
    pthread_mutex_lock(&log_mutex);
    if (log_roll) {
        struct tm *tm;
        time_t now = time(NULL);
        tm = localtime(&now);
        int day = (tm->tm_year + 1900) * 10000 + (tm->tm_mon + 1) * 100 + tm->tm_mday;
        if (!logfp || day != log_day) log_open();
    } else if (!logfp) {
        log_open();
    }
    if (logfp) { fprintf(logfp, "%s\n", line); fflush(logfp); }
    printf("%s\n", line);
    fflush(stdout);
    pthread_mutex_unlock(&log_mutex);
}

/* ---------------- periodic STAT summary ---------------- */

/* called for every accepted PPS sample (serial or gpio thread) */
static void acc_sample(long long off_us) {
    pthread_mutex_lock(&acc_mutex);
    if (acc_n == 0 || off_us < acc_off_min_us) acc_off_min_us = off_us;
    if (acc_n == 0 || off_us > acc_off_max_us) acc_off_max_us = off_us;
    acc_off_sum_us += off_us;
    acc_n++;
    pthread_mutex_unlock(&acc_mutex);
}

/* Emit one STAT summary line and reset the window.  Called from the gpio
   thread every log_interval seconds of MONOTONIC time (immune to clock
   steps, same rule as the watchdog). */
static void emit_summary(void) {
    int n, tod, g, ne, na, bt;
    long long mn, mx, avg;
    pthread_mutex_lock(&acc_mutex);
    n = acc_n; mn = acc_off_min_us; mx = acc_off_max_us;
    avg = acc_n ? acc_off_sum_us / acc_n : 0;
    tod = acc_tod_n; g = acc_gated; ne = acc_noedge; na = acc_noanchor; bt = acc_badtod;
    acc_n = 0; acc_tod_n = 0;
    acc_gated = acc_noedge = acc_noanchor = acc_badtod = 0;
    acc_off_min_us = acc_off_max_us = acc_off_sum_us = 0;
    pthread_mutex_unlock(&acc_mutex);

    char ts[32];
    struct tm tm;
    time_t now = time(NULL);
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    if (n > 0)
        logmsg("STAT: window=%ds ts=%s n=%d off_us min=%+lld avg=%+lld max=%+lld "
               "tod_n=%d gated=%d noedge=%d noanchor=%d badtod=%d",
               log_interval, ts, n, mn, avg, mx, tod, g, ne, na, bt);
    else
        logmsg("STAT: window=%ds ts=%s n=0 off_us min=- avg=- max=- "
               "tod_n=%d gated=%d noedge=%d noanchor=%d badtod=%d",
               log_interval, ts, tod, g, ne, na, bt);
}

/* ---------------- log retention ---------------- */

typedef struct { char name[64]; long long bytes; } PruneEnt;
#define PRUNE_MAX_FILES 512

/* Size-capped retention: days are sealed (gzipped) at midnight rollover
   and kept indefinitely; only when the total size of managed files
   exceeds retain_mb are the oldest days deleted until the total fits.
   The newest file (today's, still open) is never deleted.  Names that do
   not parse as a date (watchdog.log, soak_start, pps_tod_unknown...log.gz)
   are never touched. */
static void prune_old_logs(void) {
    if (retain_mb <= 0) return;
    PruneEnt ent[PRUNE_MAX_FILES];
    int n = 0, i, j;
    DIR *d = opendir(logdir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && n < PRUNE_MAX_FILES) {
        int y, mo, dd;
        if (sscanf(de->d_name, "pps_tod_%4d-%2d-%2d.log", &y, &mo, &dd) != 3) continue;
        char path[512];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", logdir, de->d_name);
        if (stat(path, &st) != 0) continue;
        snprintf(ent[n].name, sizeof(ent[n].name), "%s", de->d_name);
        ent[n].bytes = (long long)st.st_size;
        n++;
    }
    closedir(d);
    if (n <= 1) return;    /* only today's file (or nothing): nothing to prune */

    /* oldest first — names carry the date, so lexicographic order works */
    for (i = 1; i < n; i++) {
        for (j = i; j > 0 && strcmp(ent[j-1].name, ent[j].name) > 0; j--) {
            PruneEnt tmp = ent[j-1];
            ent[j-1] = ent[j];
            ent[j] = tmp;
        }
    }

    long long total = 0, cap = (long long)retain_mb * 1024 * 1024;
    for (i = 0; i < n; i++) total += ent[i].bytes;
    char pruned[512];
    size_t pl = 0;
    pruned[0] = 0;
    /* keep the newest entry (today's active log) even if it alone exceeds the cap */
    for (i = 0; i < n - 1 && total > cap; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", logdir, ent[i].name);
        if (unlink(path) == 0) {
            total -= ent[i].bytes;
            int wr = snprintf(pruned + pl, sizeof(pruned) - pl,
                              "%s%s", pl ? "," : "", ent[i].name);
            if (wr > 0) {
                pl += (size_t)wr;
                if (pl >= sizeof(pruned)) pl = sizeof(pruned) - 1;
            }
        }
    }
    if (pruned[0]) logmsg("log: size cap %d MB exceeded, pruned oldest: %s", retain_mb, pruned);
}

/* ---------------- time helpers ---------------- */
static long long now_rt_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static long long now_mono_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static long long now_mraw_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---------------- chrony SHM ---------------- */
static int shm_init(void) {
    int id = shmget(SHMKEY_BASE + shm_unit, sizeof(shmTime_t), IPC_CREAT | 0666);
    if (id < 0) { perror("shmget"); return -1; }
    shm = (shmTime_t *)shmat(id, NULL, 0);
    if (shm == (void *)-1) { perror("shmat"); return -1; }
    memset(shm, 0, sizeof(shmTime_t));
    shm->mode = 1;
    return 0;
}

static void shm_write(time_t rsec, long long rnsec, time_t csec, long long cnsec) {
    if (!shm) return;
    shm->valid = 0;
    __sync_synchronize();
    shm->count++;
    shm->clockTimeStampSec = csec;
    shm->clockTimeStampUSec = (int)(cnsec / 1000);
    shm->clockTimeStampNSec = (int)cnsec;
    shm->receiveTimeStampSec = rsec;
    shm->receiveTimeStampUSec = (int)(rnsec / 1000);
    shm->receiveTimeStampNSec = (int)rnsec;
    shm->leap = 0;
    shm->precision = -20;
    shm->nsamples = 0;
    __sync_synchronize();
    shm->count++;
    shm->valid = 1;
}

/* ---------------- pairing state ---------------- */
static pthread_mutex_t pair_mutex = PTHREAD_MUTEX_INITIALIZER;
static int anchor_valid = 0;
static time_t anchor_S = 0;
static long long anchor_mono = 0;   /* sentence arrival, mono ns */
static long long edge_count = 0;    /* edges sampled since last anchor */
static long long pending[8];        /* unconsumed edge mono timestamps */
static int pending_n = 0;
static int ts_is_realtime = -1;     /* clock domain of gpio event timestamps */
static int mode_ntp = 0;            /* -A ntp: assign seconds from system clock */
static int edge_delta = 1;          /* next edge after sentence = anchor+delta */
/* Coarse-fallback safety state (all touched only by the gpio thread) */
static int strike = 0;              /* consecutive gated samples */
static int strike_max = 10;         /* -M: forced step after this many */
static int forced_resets = 0;       /* steps taken in the current hour */
static long long forced_window_mono = 0;
static int reset_max = 6;           /* -X: steps allowed per hour */
static int reset_capped = 0;        /* 1 = over quota, clock writes disabled */
static long long capped_log_mono = 0;
static int anchor_seq = 0;          /* consecutive anchors advancing by 1s */
static time_t anchor_prev_S = 0;    /* previous anchor second */
static int strike_sign = 0;         /* sign of the current gated run */

/* ---------------- NMEA TOD parsing ---------------- */
static int getnth(char *line, int n, char *out, int outsz) {
    char *p = line; int i;
    for (i = 0; i < n; i++) {
        p = strchr(p, ',');
        if (!p) return -1;
        p++;
    }
    char *end = strchr(p, ',');
    int len = end ? (int)(end - p) : (int)strlen(p);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, p, len); out[len] = 0;
    return 0;
}

/* returns UTC epoch seconds, or 0 if not a valid time sentence */
static time_t parse_line(char *line) {
    char f[32];
    if (line[0] != '$') return 0;
    if (getnth(line, 0, f, sizeof(f)) < 0) return 0;
    int is_rmc = (strlen(f) >= 6 && strncmp(f + 3, "RMC", 3) == 0);
    int is_zda = (strlen(f) >= 6 && strncmp(f + 3, "ZDA", 3) == 0);
    if (!is_rmc && !is_zda) return 0;

    int hh = 0, mi = 0, ss = 0, dd = 0, mo = 0, yr = 0;
    char tmp[16];
    if (is_rmc) {
        /* $--RMC,hhmmss.ss,A,lat,N,lon,E,spd,cog,ddmmyy,.. */
        if (getnth(line, 1, tmp, sizeof(tmp)) < 0) return 0;
        if (strlen(tmp) < 6) return 0;
        hh = (tmp[0] - '0') * 10 + (tmp[1] - '0');
        mi = (tmp[2] - '0') * 10 + (tmp[3] - '0');
        ss = (tmp[4] - '0') * 10 + (tmp[5] - '0');
        if (getnth(line, 2, tmp, sizeof(tmp)) < 0) return 0;
        if (tmp[0] != 'A' && tmp[0] != 'a') return 0;   /* fix status */
        if (getnth(line, 9, tmp, sizeof(tmp)) < 0) return 0;
        if (strlen(tmp) < 6) return 0;
        dd = (tmp[0] - '0') * 10 + (tmp[1] - '0');
        mo = (tmp[2] - '0') * 10 + (tmp[3] - '0');
        yr = (tmp[4] - '0') * 10 + (tmp[5] - '0');      /* 20yy */
    } else {
        /* $--ZDA,hhmmss.ss,dd,mm,yyyy,.. */
        if (getnth(line, 1, tmp, sizeof(tmp)) < 0) return 0;
        if (strlen(tmp) < 6) return 0;
        hh = (tmp[0] - '0') * 10 + (tmp[1] - '0');
        mi = (tmp[2] - '0') * 10 + (tmp[3] - '0');
        ss = (tmp[4] - '0') * 10 + (tmp[5] - '0');
        if (getnth(line, 2, tmp, sizeof(tmp)) < 0) return 0;
        dd = atoi(tmp);
        if (getnth(line, 3, tmp, sizeof(tmp)) < 0) return 0;
        mo = atoi(tmp);
        if (getnth(line, 4, tmp, sizeof(tmp)) < 0) return 0;
        yr = atoi(tmp);
    }
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = is_rmc ? (yr + 100) : (yr - 1900);
    t.tm_mon = mo - 1;
    t.tm_mday = dd;
    t.tm_hour = hh;
    t.tm_min = mi;
    t.tm_sec = ss;
    time_t S = timegm(&t);
    /* sanity window: unlocked/fake receivers can emit absurd dates; do not
       anchor on them (counted, visible in status for diagnosis) */
    struct tm chk;
    gmtime_r(&S, &chk);
    int y = chk.tm_year + 1900;
    if (y < 2020 || y > 2100) {
        __sync_fetch_and_add(&sec_badtod, 1);
        return 0;
    }
    return S;
}

/* ---------------- sample emission ---------------- */
/* event timestamp -> CLOCK_REALTIME nanoseconds */
static long long edge_realtime(long long ev_ts) {
    if (ts_is_realtime != 1)
        return ev_ts + (now_rt_ns() - now_mono_ns());
    return ev_ts;
}

/* a consistent ~1s gate deviation means our second assignment is off by 1 */
static int off_is_onesec(long long off_ns) {
    long long a = llabs(off_ns);
    return a > 650000000LL && a < 1350000000LL;
}
static void emit_sample(time_t S, long long e_mono, int *sampled) {
    long long rt_e;
    if (ts_is_realtime == 1) {
        rt_e = e_mono;
    } else {
        rt_e = e_mono + (now_rt_ns() - now_mono_ns());
    }
    long long off = (long long)S * 1000000000LL - rt_e;
    last_off_us = off / 1000;
    last_off_mono = now_mono_ns();   /* every edge, gated or not */
    if (llabs(off) > 500000000LL) {            /* clock too far off: gate */
        __sync_fetch_and_add(&sec_gated, 1);
        {
            int sg = (off > 0) ? 1 : -1;
            if (strike_sign && sg != strike_sign) strike = 0;  /* alternating
                offset = the second assignment is oscillating, not the clock;
                stepping on it would chase its own tail. */
            strike_sign = sg;
        }
        strike++;
        logmsg("PPS: sample gated (clock off %.3fs, consec=%d)",
               (double)off / 1e9, strike);
        *sampled = 0;
        return;
    }
    strike = 0;                /* a healthy sample clears the anomaly run */
    strike_sign = 0;
    /* SHM convention (chrony 3.4): receiveTimeStamp = local time when the
       sample was taken, clockTimeStamp = reference/true time. offset =
       clock - receive = S - rt_e = the clock error (correct sign). */
    shm_write((time_t)(rt_e / 1000000000LL), rt_e % 1000000000LL, S, 0);
    last_sample_mono = now_mono_ns();
    *sampled = 1;
    if (log_interval == 0)
        logmsg("PPS: S=%lld edge_rt=%.6f offset=%.3f ms",
               (long long)S, (double)rt_e / 1e9, (double)off / 1e6);
    else
        acc_sample(last_off_us);
}

/* ---------------- per-second status file (for the watchdog) ---------------- */
static void status_update(void) {
    if (!status_enabled) return;
    static int dir_done = 0;
    char tmp[160], buf[512];
    long long now_m = now_mono_ns();
    long long age = last_sample_mono ? (now_m - last_sample_mono) / 1000000000LL : -1;
    /* per-second clock divergence: realtime vs MONOTONIC_RAW since the last
       status frame. ~0 when healthy; a jump when the clock was stepped
       (bootstrap/external date), ~-1e6 us/s when the clock stops advancing. */
    long long rt_now = now_rt_ns(), mraw_now = now_mraw_ns();
    long long diag = prev_rt_ns ? ((rt_now - prev_rt_ns) - (mraw_now - prev_mraw_ns)) / 1000 : 0;
    prev_rt_ns = rt_now;
    prev_mraw_ns = mraw_now;
    int anchor_fresh = 0;
    pthread_mutex_lock(&pair_mutex);
    anchor_fresh = anchor_valid && (now_m - anchor_mono < 3000000000LL);
    pthread_mutex_unlock(&pair_mutex);
    if (!dir_done) {
        char *slash = strrchr(status_path, '/');
        if (slash && slash != status_path) {
            char dir[128];
            int n = (int)(slash - status_path);
            if (n >= (int)sizeof(dir)) n = sizeof(dir) - 1;
            memcpy(dir, status_path, n);
            dir[n] = 0;
            mkdir(dir, 0755);
        }
        dir_done = 1;
    }
    struct tm tm;
    time_t now_rt = time(NULL);
    localtime_r(&now_rt, &tm);
    int n = snprintf(buf, sizeof(buf),
        "ts=%04d-%02d-%02d %02d:%02d:%02d\n"
        "good=%d\n"
        "last_good_age_s=%lld\n"
        "offset_us=%lld\n"
        "anchor_fresh=%d\n"
        "sec_gated=%d\n"
        "sec_noedge=%d\n"
        "sec_noanchor=%d\n"
        "sec_badtod=%d\n"
        "rt_vs_mono_us=%lld\n"
        "anchor_seq=%d\n"
        "consec_gated=%d\n"
        "forced_resets=%d\n"
        "reset_state=%s\n",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        (age >= 0 && age <= 3) ? 1 : 0,
        age, last_off_us, anchor_fresh,
        sec_gated, sec_noedge, sec_noanchor, sec_badtod, diag,
        anchor_seq, strike, forced_resets, reset_capped ? "CAPPED" : "OK");
    snprintf(tmp, sizeof(tmp), "%s.tmp", status_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)write(fd, buf, (size_t)n);
        close(fd);
        rename(tmp, status_path);
    }
    if (log_interval > 0) {
        /* fold this second's counters into the summary window */
        pthread_mutex_lock(&acc_mutex);
        acc_gated += sec_gated;
        acc_noedge += sec_noedge;
        acc_noanchor += sec_noanchor;
        acc_badtod += sec_badtod;
        pthread_mutex_unlock(&acc_mutex);
    }
    sec_gated = sec_noedge = sec_noanchor = sec_badtod = 0;
}

/* ---------------- shared stop flag ---------------- */
static volatile int signal_stop = 0;

static void on_signal(int sig) { (void)sig; signal_stop = 1; }

/* ---------------- serial TOD thread ---------------- */
static char tod_dev[64] = "/dev/ttyS3";
static speed_t tod_baud = B9600;

static void *serial_thread(void *arg) {
    (void)arg;
    int fd = open(tod_dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { logmsg("TOD: open %s failed: %s", tod_dev, strerror(errno)); return NULL; }
    struct termios opt;
    memset(&opt, 0, sizeof(opt));
    if (tcgetattr(fd, &opt) < 0) { perror("tcgetattr"); close(fd); return NULL; }
    cfmakeraw(&opt);
    opt.c_cflag |= CLOCAL | CREAD;
    opt.c_cc[VMIN] = 0;
    opt.c_cc[VTIME] = 0;
    cfsetispeed(&opt, tod_baud);
    cfsetospeed(&opt, tod_baud);
    tcsetattr(fd, TCSANOW, &opt);
    tcflush(fd, TCIFLUSH);
    logmsg("TOD: opened %s", tod_dev);

    char line[256]; int linelen = 0;
    char buf[128];
    struct pollfd pfd = { fd, POLLIN, 0 };
    for (;;) {
        if (signal_stop) break;
        int pr = poll(&pfd, 1, 1000);
        if (pr <= 0) continue;
        int n = read(fd, buf, sizeof(buf));
        if (n <= 0) continue;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (linelen > 0) {
                    line[linelen] = 0;
                    time_t S = parse_line(line);
                    if (S > 0) {
                        pthread_mutex_lock(&pair_mutex);
                        anchor_S = S;
                        anchor_mono = now_mono_ns();
                        anchor_valid = 1;
                        /* A reference that JUMPS must not be written into the
                         * clock: only a stream of consecutive seconds is
                         * trustworthy enough for a coarse step. */
                        if (anchor_prev_S && S == anchor_prev_S + 1) {
                            if (anchor_seq < 1000000) anchor_seq++;
                        } else {
                            anchor_seq = 1;
                        }
                        anchor_prev_S = S;
                        /* flush: the pending edge just before arrival is edge_S */
                        long long best_e = 0;
                        int have_e = 0;
                        for (int k = 0; k < pending_n; k++) {
                            long long e = pending[k];
                            if (e > anchor_mono) continue;          /* future edge */
                            if (anchor_mono - e > 1000000000LL) continue; /* stale */
                            if (!have_e || e > best_e) { best_e = e; have_e = 1; }
                        }
                        pending_n = 0;
                        if (have_e) {
                            /* detect receiver phase lag at sentence level */
                            long long rt_e = edge_realtime(best_e);
                            long long off_ns = (long long)anchor_S * 1000000000LL - rt_e;
                            if (off_is_onesec(off_ns))
                                logmsg("TOD: sentence +1s phase-lag detected");
                            int sampled = 0;
                            emit_sample(anchor_S, best_e, &sampled);
                        }
                        edge_count = 0;
                        if (log_interval == 0) {
                            logmsg("TOD: anchor S=%lld (%s)", (long long)anchor_S, line);
                        } else {
                            pthread_mutex_lock(&acc_mutex);
                            acc_tod_n++;
                            pthread_mutex_unlock(&acc_mutex);
                        }
                        pthread_mutex_unlock(&pair_mutex);
                    }
                    linelen = 0;
                }
            } else {
                if (linelen < (int)sizeof(line) - 1) line[linelen++] = c;
                else linelen = 0;                                   /* overflow */
            }
        }
    }
    return NULL;
}

/* ---------------- GPIO capture thread ---------------- */
static int gpio_chip = 3, gpio_line = 5;
static int edge_flags = GPIOEVENT_REQUEST_RISING_EDGE;

static void *gpio_thread(void *arg) {
    (void)arg;
    char path[64];
    snprintf(path, sizeof(path), "/dev/gpiochip%d", gpio_chip);
    int cfd = open(path, O_RDONLY);
    if (cfd < 0) { logmsg("GPIO: open %s failed", path); return NULL; }
    struct gpioevent_request req;
    memset(&req, 0, sizeof(req));
    req.lineoffset = gpio_line;
    req.handleflags = GPIOHANDLE_REQUEST_INPUT;
    req.eventflags = edge_flags;
    strncpy(req.consumer_label, "pps-tod", sizeof(req.consumer_label) - 1);
    if (ioctl(cfd, GPIO_GET_LINEEVENT_IOCTL, &req) < 0) {
        logmsg("GPIO: event request failed: %s", strerror(errno));
        close(cfd);
        return NULL;
    }
    close(cfd);
    logmsg("GPIO: chip%d line%d events OK", gpio_chip, gpio_line);

    struct pollfd pfd = { req.fd, POLLIN, 0 };
    struct gpioevent_data ev;
    long long last_print = 0;
    while (!signal_stop) {
        /* Coarse fallback.  Two triggers, both requiring a reference stream
           that looks sane (anchor_seq >= 3 consecutive advancing seconds):
             urgent - the clock is more than 1.0s off (dead RTC, years off)
             stuck  - the 0.5s sample gate has swallowed strike_max samples
                      in a row -- the old dead band, where a 0.5..1.0s error
                      produced neither SHM samples nor any correction.
           The step is by the measured offset, so sub-second phase survives. */
        if (coarse_enabled) {
            pthread_mutex_lock(&pair_mutex);
            int fresh = anchor_valid && (now_mono_ns() - anchor_mono < 3000000000LL);
            int seq = anchor_seq;
            pthread_mutex_unlock(&pair_mutex);

            /* Step by the offset the EDGE measured (S - edge_local_time), not
               by anchor_S - now: the sentence arrives somewhere inside its
               second, so the latter carries 0..1s of arrival phase and would
               chase the phase instead of the clock error. */
            int meas = (now_mono_ns() - last_off_mono < 3000000000LL);
            long long off = meas ? (long long)last_off_us * 1000LL : 0;

            int urgent = fresh && meas && llabs(off) > 1000000000LL;
            int stuck  = fresh && meas && strike >= strike_max;

            if (seq >= 3 && (urgent || stuck) &&
                now_mono_ns() - last_nudge_mono > 10000000000LL) {
                if (now_mono_ns() - forced_window_mono >= 3600000000000LL) {
                    forced_window_mono = now_mono_ns();
                    forced_resets = 0;
                    reset_capped = 0;
                }
                last_nudge_mono = now_mono_ns();
                if (forced_resets >= reset_max) {
                    if (!reset_capped || now_mono_ns() - capped_log_mono > 3600000000000LL) {
                        reset_capped = 1;
                        capped_log_mono = now_mono_ns();
                        logmsg("PPS: coarse step quota reached (%d/h) - clock writes "
                               "disabled; reference or clock is faulty, needs manual check",
                               reset_max);
                    }
                } else {
                    struct timespec bts;
                    /* Snap to the reference's integer second rather than
                       applying `off` as a delta.  The measured offset comes
                       from round(edge_local_time), so it carries an
                       unavoidable +-0.5s quantisation; applying it as a delta
                       would preserve whatever sub-second phase the clock
                       happened to have (and can latch it ~0.5s off).  Setting
                       nsec=0 forces the phase back onto the true second, which
                       is what makes the chain absolutely correct rather than
                       merely self-consistent. */
                    pthread_mutex_lock(&pair_mutex);
                    bts.tv_sec = anchor_S + edge_delta - 1;  /* true second */
                    pthread_mutex_unlock(&pair_mutex);
                    bts.tv_nsec = 0;
                    clock_settime(CLOCK_REALTIME, &bts);
                    forced_resets++;
                    strike = 0;
                    logmsg("PPS: %s clock step by %+.3fs (anchor_seq=%d off_target=%lld resets=%d/h)",
                           urgent ? "coarse" : "forced", (double)off / 1e9,
                           seq, (long long)bts.tv_sec, forced_resets);
                }
            }
        }
        if (now_mono_ns() - last_status_mono >= 1000000000LL) {
            last_status_mono = now_mono_ns();
            if (status_enabled) status_update();
            if (log_interval > 0 &&
                now_mono_ns() - last_summary_mono >= (long long)log_interval * 1000000000LL) {
                last_summary_mono = now_mono_ns();
                emit_summary();
            }
            /* retention: once per local day (first tick covers startup) */
            {
                time_t now = time(NULL);
                struct tm tm;
                localtime_r(&now, &tm);
                int today = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
                if (last_prune_day != today) {
                    last_prune_day = today;
                    prune_old_logs();
                }
            }
        }
        int pr = poll(&pfd, 1, 1000);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) {
            __sync_fetch_and_add(&sec_noedge, 1);
            if (now_mono_ns() - last_print > 10000000000LL) {
                last_print = now_mono_ns();
                logmsg("PPS: idle, no edges (anchor_valid=%d)", anchor_valid);
            }
            continue;
        }
        int n = read(req.fd, &ev, sizeof(ev));
        if (n != (int)sizeof(ev)) continue;
        if (ev.id != GPIOEVENT_EVENT_RISING_EDGE &&
            ev.id != GPIOEVENT_EVENT_FALLING_EDGE) continue;

        if (ts_is_realtime < 0) {
            long long rt = now_rt_ns(), mono = now_mono_ns();
            ts_is_realtime = (llabs((long long)ev.timestamp - rt) <
                              llabs((long long)ev.timestamp - mono)) ? 1 : 0;
            logmsg("GPIO: event ts clock = %s", ts_is_realtime ? "REALTIME" : "MONOTONIC");
        }

        pthread_mutex_lock(&pair_mutex);
        if (mode_ntp) {
            /* NTP-mode: absolute seconds come from the (NTP-synced) system
               clock; PPS provides the sub-second discipline. The round()
               assignment measures the clock's sub-second offset, like a
               freerun PPS refclock. */
            long long rt_e = edge_realtime(ev.timestamp);
            long long r = rt_e / 1000000000LL;
            /* ok if |fraction| < 0.5s (always by rounding); require the
               clock to be roughly sane: the edge must be near a boundary */
            long long frac = rt_e - r * 1000000000LL;
            if (frac > 400000000LL && frac < 600000000LL)
                continue;   /* edge mid-second: not a real 1PPS boundary */
            int sampled = 0;
            emit_sample((time_t)(r + (frac > 500000000LL ? 1 : 0)),
                        ev.timestamp, &sampled);
        } else if (anchor_valid && now_mono_ns() - anchor_mono < 3000000000LL) {
            /* TOD-mode: primary second assignment = round(edge realtime),
               immune to the sentence-arrival phase. The anchor chain only
               arbitrates the integer second and sets the absolute frame
               (they agree once the clock is right). */
            long long rt_e = edge_realtime(ev.timestamp);
            long long r = rt_e / 1000000000LL;
            long long f = rt_e % 1000000000LL;
            time_t S_round = (time_t)(r + (f >= 500000000LL ? 1 : 0));
            time_t S_chain = anchor_S + edge_delta + (time_t)edge_count;
            time_t S;
            if (llabs((long long)S_round - (long long)S_chain) <= 1) {
                S = S_round;
            } else {
                S = S_chain;
                logmsg("PPS: anchor sanity override (S_round=%lld chain=%lld)",
                       (long long)S_round, (long long)S_chain);
            }
            edge_count++;
            int sampled = 0;
            emit_sample(S, ev.timestamp, &sampled);
        } else if (pending_n < 8) {
            /* no fresh anchor: queue for the next sentence to flush */
            __sync_fetch_and_add(&sec_noanchor, 1);
            pending[pending_n++] = ev.timestamp;
        }
        pthread_mutex_unlock(&pair_mutex);
    }
    close(req.fd);
    logmsg("GPIO: thread exit");
    return NULL;
}

/* ---------------- main ---------------- */
int main(int argc, char **argv) {
    int daemonize = 1;
    int c;
    while ((c = getopt(argc, argv, "g:e:t:b:u:l:s:CDA:SL:R:M:X:")) != -1) {
        switch (c) {
        case 'g': sscanf(optarg, "%d:%d", &gpio_chip, &gpio_line); break;
        case 'e':
            if (!strcmp(optarg, "rising")) edge_flags = GPIOEVENT_REQUEST_RISING_EDGE;
            else if (!strcmp(optarg, "falling")) edge_flags = GPIOEVENT_REQUEST_FALLING_EDGE;
            else if (!strcmp(optarg, "both")) edge_flags = GPIOEVENT_REQUEST_BOTH_EDGES;
            break;
        case 't': snprintf(tod_dev, sizeof(tod_dev), "%s", optarg); break;
        case 'b':
            tod_baud = atoi(optarg) == 115200 ? B115200 :
                       atoi(optarg) == 4800 ? B4800 :
                       atoi(optarg) == 19200 ? B19200 : B9600;
            break;
        case 'u': shm_unit = atoi(optarg); break;
        case 'l': snprintf(logfile, sizeof(logfile), "%s", optarg); log_roll = 0; break;
        case 's': snprintf(status_path, sizeof(status_path), "%s", optarg); break;
        case 'S': status_enabled = 0; break;
        case 'C': coarse_enabled = 0; break;
        case 'A': mode_ntp = !strcmp(optarg, "ntp"); break;
        case 'D': daemonize = 0; break;
        case 'L':
            log_interval = atoi(optarg);
            if (log_interval < 0) log_interval = 0;
            break;
        case 'R':
            retain_mb = atoi(optarg);
            if (retain_mb < 0) retain_mb = 0;
            break;
        case 'M':                       /* consecutive gated samples -> step */
            strike_max = atoi(optarg);
            if (strike_max < 1) strike_max = 1;
            break;
        case 'X':                       /* coarse steps allowed per hour */
            reset_max = atoi(optarg);
            if (reset_max < 1) reset_max = 1;
            break;
        default:
            fprintf(stderr, "usage: %s [-g chip:line] [-e rising|falling|both] "
                    "[-t dev] [-b baud] [-u unit] [-l logfile] "
                    "[-s statfile] [-L summary_s] [-R retain_mb] "
                    "[-M gated_run] [-X steps_per_hour] "
                    "[-A ntp] [-C] [-D] [-S]\n", argv[0]);
            return 1;
        }
    }

    log_open();
    if (!logfp) { perror("logfile"); return 1; }

    if (shm_init() < 0) return 1;
    last_nudge_mono = now_mono_ns() - 10000000000LL;   /* nudge eligible immediately */
    last_summary_mono = now_mono_ns();                 /* first STAT after log_interval */
    logmsg("pps_tod start: gpio=%d:%d tod=%s shm_unit=%d",
           gpio_chip, gpio_line, tod_dev, shm_unit);

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    if (daemonize) {
        pid_t pid = fork();
        if (pid < 0) return 1;
        if (pid > 0) {
            FILE *pf = fopen("/tmp/pps_tod.pid", "w");
            if (pf) { fprintf(pf, "%d\n", pid); fclose(pf); }
            return 0;
        }
        setsid();
    }

    pthread_t t1, t2;
    pthread_create(&t1, NULL, serial_thread, NULL);
    pthread_create(&t2, NULL, gpio_thread, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    logmsg("pps_tod exit");
    return 0;
}

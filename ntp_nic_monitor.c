/* ntp_nic_monitor.c — per-interface NTP serving monitor for the
 * 「服务监控及报警」tab.
 *
 * Why this exists: chrony 3.4 keeps only global server counters and has no
 * per-interface view, so "which port is being asked for time, by whom, and
 * is it answering" cannot be answered from chronyd alone.
 *
 * Two data paths, because the kernel delivers them differently:
 *  - Requests + client identity: AF_PACKET (SOCK_DGRAM, ETH_P_IP) inbound
 *    capture.  sll_pkttype tells inbound from locally generated packets —
 *    required on 4.19, which lacks PACKET_IGNORE_OUTGOING (4.20+).
 *  - Responses: iptables OUTPUT counters per interface.  Outgoing packets
 *    are NOT delivered to packet sockets on this board's kernel (verified:
 *    9 requests in, 9 responses sent per iptables, 0 seen by AF_PACKET),
 *    so counting them has to happen in the netfilter path instead.
 *
 * Output: /var/db/ntp_nic.json, rewritten atomically every -i seconds.
 * Memory is bounded and allocation-free after startup: fixed hash table for
 * clients (linear probing), fixed per-interface slots, fixed 60-minute ring.
 *
 * usage: ntp_nic_monitor [-f out.json] [-i secs] [-n nic1,nic2,...] [-C] [-D]
 *   -f  output JSON path        (default /var/db/ntp_nic.json)
 *   -i  write interval seconds  (default 5)
 *   -n  interfaces to track     (default eth0,eth1,eth2,eth3)
 *   -C  do not touch iptables   (response counts left at 0)
 *   -D  stay in foreground      (default when not started by systemd)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <arpa/inet.h>

#define MAX_NICS     8
#define CLIENT_SLOTS 1024          /* power of two: (i,ip) open addressing */
/* The UI offers 1m / 1h / 5h / 24h, so the rings must cover the LARGEST window;
   the smaller ones are summed from a suffix of the same ring. */
#define BUCKETS      1440          /* 24 h of one-minute buckets */
#define BUCKETS_OUT  60            /* only the trailing hour is published */
#define CLIENT_TTL   86400         /* keep clients for the widest window; the
                                    * UI filters by the selected range */
#define NWINDOWS     4
static const int g_windows[NWINDOWS]     = { 60, 3600, 18000, 86400 };
static const char *g_winsfx[NWINDOWS]    = { "1m", "1h", "5h", "24h" };
#define TOP_CLIENTS  32            /* per interface, by request count */
#define IPT_OUT      32768         /* buffer for `iptables -nvw -L OUTPUT` */
#define SSEC         120           /* trailing seconds published per NIC: the
                                    * UI's 1-minute view plots per-second
                                    * samples, so the ring must reach back
                                    * past a full minute (2x margin). */

typedef struct {
    int used;
    int ifindex;
    unsigned int ip;               /* network byte order */
    unsigned long long req;
    unsigned long long valid;      /* requests whose payload looked like NTP mode 3 */
    time_t last;
} ClientEnt;

typedef struct {
    int used;
    char name[IFNAMSIZ];
    int ifindex;
    unsigned long long req;        /* inbound udp/123 packets */
    unsigned long long valid;      /* of which parse as an NTP client request */
    unsigned long long rsp;        /* iptables OUTPUT udp/123 packets */
    unsigned long long ipt_rule;   /* iptables counter when the daemon started */
    unsigned long long ipt_last;   /* previous iptables reading (for deltas) */
    unsigned int bucket[BUCKETS];  /* requests  per minute, ring by epoch-minute */
    unsigned int vbucket[BUCKETS]; /* valid NTP per minute */
    unsigned int rbucket[BUCKETS]; /* responses per minute */
    long bucket_min;               /* the minute the newest bucket belongs to */
    unsigned int sreq[SSEC];       /* requests per second, ring by epoch-second */
    long sreq_min;                 /* the newest second ever written (0 = none) */
} NicStat;

static NicStat   g_nics[MAX_NICS];
static int       g_nnic = 0;
static ClientEnt g_clients[CLIENT_SLOTS];

static const char *out_path = "/var/db/ntp_nic.json";
static int interval = 5;
static int use_iptables = 1;
static volatile int stop_flag = 0;

static void logmsg(const char *fmt, ...) {
    va_list ap;
    time_t now = time(NULL);
    struct tm tm;
    char ts[32];
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    fprintf(stderr, "%s ", ts);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void on_signal(int sig) { (void)sig; stop_flag = 1; }

static long now_min(void) { return (long)(time(NULL) / 60); }

/* ── interface table ───────────────────────────────────────────────── */

static int nic_slot(int ifindex) {
    int i;
    for (i = 0; i < g_nnic; i++)
        if (g_nics[i].used && g_nics[i].ifindex == ifindex) return i;
    return -1;
}

static void add_nic(const char *name) {
    int i, idx;
    if (g_nnic >= MAX_NICS) return;
    for (i = 0; i < g_nnic; i++)
        if (g_nics[i].used && strcmp(g_nics[i].name, name) == 0) return;
    idx = if_nametoindex(name);
    if (idx == 0) {
        logmsg("NIC %s: no such interface, skipped", name);
        return;
    }
    for (i = 0; i < MAX_NICS; i++) {
        if (!g_nics[i].used) {
            memset(&g_nics[i], 0, sizeof(g_nics[i]));
            snprintf(g_nics[i].name, sizeof(g_nics[i].name), "%s", name);
            g_nics[i].ifindex = idx;
            g_nics[i].used = 1;
            g_nics[i].bucket_min = now_min();
            g_nnic++;
            return;
        }
    }
}

/* ── per-client table (open addressing, bounded) ───────────────────── */

static ClientEnt *client_get(int ifindex, unsigned int ip, time_t now) {
    unsigned int h = (unsigned int)ifindex * 2654435761u ^ ip;
    unsigned int i, slot;
    for (i = 0; i < CLIENT_SLOTS; i++) {
        slot = (h + i) & (CLIENT_SLOTS - 1);
        if (!g_clients[slot].used) {
            g_clients[slot].used = 1;
            g_clients[slot].ifindex = ifindex;
            g_clients[slot].ip = ip;
            g_clients[slot].req = 0;
            g_clients[slot].valid = 0;
            g_clients[slot].last = now;
            return &g_clients[slot];
        }
        if (g_clients[slot].ifindex == ifindex && g_clients[slot].ip == ip)
            return &g_clients[slot];
    }
    return NULL;    /* table full: drop rather than grow */
}

static void clients_expire(time_t now) {
    int i;
    for (i = 0; i < CLIENT_SLOTS; i++)
        if (g_clients[i].used && now - g_clients[i].last > CLIENT_TTL)
            g_clients[i].used = 0;
}

/* ── capture ───────────────────────────────────────────────────────── */

/* Roll the per-minute rings forward to minute m, clearing the skipped
 * minutes.  Shared by the request and response paths so either traffic type
 * keeps the window current. */
static void bucket_advance(NicStat *ns, long m) {
    long d, k;
    if (m == ns->bucket_min) return;
    d = m - ns->bucket_min;
    if (d < 0 || d > BUCKETS) d = BUCKETS;   /* clock jump: clear everything */
    for (k = 1; k <= d; k++) {
        long i = (ns->bucket_min + k) % BUCKETS;
        ns->bucket[i] = ns->vbucket[i] = ns->rbucket[i] = 0;
    }
    ns->bucket_min = m;
}

/* Sum one ring over the trailing `mins` minutes, masking slots that have aged
 * out (bucket_min only moves when traffic arrives).  Walking just the wanted
 * suffix keeps a 24h ring cheap to query every few seconds. */
static unsigned int bucket_sum(const unsigned int *arr, long bucket_min, long m, int mins) {
    unsigned int s = 0;
    int k;
    if (mins > BUCKETS) mins = BUCKETS;
    for (k = 0; k < mins; k++) {
        long bm = m - (mins - 1) + k;
        if (bm <= bucket_min && bucket_min - bm < BUCKETS) s += arr[bm % BUCKETS];
    }
    return s;
}

/* Roll the per-second request ring forward to second `s`, clearing skipped
 * seconds (idle or post-gap) the same way bucket_advance does for minutes —
 * without this, a slot would keep serving the count from exactly SSEC
 * seconds earlier as if it belonged to the current second. */
static void sec_advance(NicStat *ns, long s) {
    long d, k;
    if (s == ns->sreq_min) return;
    d = s - ns->sreq_min;
    if (ns->sreq_min <= 0) {          /* first packet ever: ring is all-zero */
        ns->sreq_min = s;
        return;
    }
    if (d < 0 || d > SSEC) d = SSEC;  /* clock jump: clear everything */
    for (k = 1; k <= d; k++) ns->sreq[(ns->sreq_min + k) % SSEC] = 0;
    ns->sreq_min = s;
}

static void account(NicStat *ns, unsigned int src_ip, int is_valid, time_t now) {
    long m = now / 60;
    ClientEnt *c;
    ns->req++;
    if (is_valid) ns->valid++;
    bucket_advance(ns, m);
    ns->bucket[m % BUCKETS]++;
    if (is_valid) ns->vbucket[m % BUCKETS]++;
    {
        long s = (long)now;
        sec_advance(ns, s);
        ns->sreq[s % SSEC]++;
    }
    c = client_get(ns->ifindex, src_ip, now);
    if (c) {
        c->req++;
        if (is_valid) c->valid++;
        c->last = now;
    }
}

/* returns 1 if the packet is an NTP client request (mode 3) */
static int ntp_mode3(const unsigned char *udp, int len) {
    if (len < 1) return 0;
    return (udp[0] & 0x07) == 3;
}

/* Drain everything currently queued, then return so the caller can do its
 * periodic work.  The socket must be non-blocking: a blocking recvfrom here
 * would park the process in this loop and no JSON would ever be written. */
static void capture_drain(int fd) {
    static unsigned char buf[2048];
    for (;;) {
        struct sockaddr_ll sa;
        socklen_t sl = sizeof(sa);
        ssize_t n;
        int ihl, slot;
        const unsigned char *ip, *udp;
        unsigned short dport;
        int udp_len;
        unsigned int src_ip;

        if (stop_flag) return;
        n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&sa, &sl);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;  /* drained */
            logmsg("recvfrom: %s", strerror(errno));
            return;
        }
        if (sa.sll_pkttype == PACKET_OUTGOING) continue;   /* locally generated */
        if (n < 28 || (buf[0] >> 4) != 4) continue;        /* IPv4 only */
        ihl = (buf[0] & 0x0F) * 4;
        if (ihl < 20 || n < ihl + 8) continue;
        if (buf[9] != 17) continue;                        /* UDP */
        ip = buf;
        udp = buf + ihl;
        udp_len = (int)n - ihl;
        if (udp_len < 8) continue;
        dport = (unsigned short)((udp[2] << 8) | udp[3]);
        if (dport != 123) continue;
        slot = nic_slot(sa.sll_ifindex);
        if (slot < 0) continue;
        memcpy(&src_ip, ip + 12, 4);                       /* avoid unaligned load */
        account(&g_nics[slot], src_ip, ntp_mode3(udp + 8, udp_len - 8), time(NULL));
    }
}

/* ── iptables OUTPUT counters ──────────────────────────────────────── */

/* fork/exec (no shell), slurp stdout, report the child's exit status.
 * status may be NULL.  Returns bytes read, or -1 on spawn failure. */
static int run_capture(char *const argv[], char *out, size_t outlen, int *status) {
    int pfd[2];
    pid_t pid;
    size_t used = 0;
    int wstat = 0;
    if (out && outlen) out[0] = '\0';
    if (pipe(pfd) < 0) return -1;
    pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        dup2(pfd[1], STDOUT_FILENO);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        close(pfd[0]); close(pfd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    while (out && used < outlen - 1) {
        ssize_t r = read(pfd[0], out + used, outlen - 1 - used);
        if (r <= 0) break;
        used += (size_t)r;
    }
    if (out) out[used] = '\0';
    close(pfd[0]);
    while (waitpid(pid, &wstat, 0) < 0 && errno == EINTR) ;
    if (status) {
        if (WIFEXITED(wstat)) *status = WEXITSTATUS(wstat);
        else *status = -1;
    }
    return (int)used;
}

/* Create the per-interface counting rules idempotently: `-C` first (exit 0
 * = present), insert only when absent.  Inserting blindly on every start
 * would stack duplicate rules and double-count the responses. */
static void ipt_setup(void) {
    int i, rc;
    char sink[64];
    for (i = 0; i < g_nnic; i++) {
        char *chk[] = { "iptables", "-w", "-C", "OUTPUT", "-o", g_nics[i].name,
                        "-p", "udp", "--sport", "123", "-j", "ACCEPT", NULL };
        char *ins[] = { "iptables", "-w", "-I", "OUTPUT", "-o", g_nics[i].name,
                        "-p", "udp", "--sport", "123", "-j", "ACCEPT", NULL };
        rc = -1;
        if (run_capture(chk, sink, sizeof(sink), &rc) < 0) continue;
        if (rc == 0) continue;                       /* already there */
        if (run_capture(ins, sink, sizeof(sink), NULL) < 0)
            logmsg("iptables insert failed for %s", g_nics[i].name);
        else
            logmsg("counting rule added: OUTPUT -o %s udp spt:123", g_nics[i].name);
    }
}

/* Read `iptables -nvx -L OUTPUT` and return the packet count of the rule
 * matching "ACCEPT udp -- * <nic> ... spt:123", or -1 when absent. */
static long long ipt_out_count(const char *nic) {
    static char out[IPT_OUT];
    char *argv[] = { "iptables", "-w", "-nvx", "-L", "OUTPUT", NULL };
    char *p, *line;
    long long value = -1;
    if (run_capture(argv, out, sizeof(out), NULL) <= 0) return -1;
    for (line = strtok_r(out, "\n", &p); line; line = strtok_r(NULL, "\n", &p)) {
        char *tok, *save;
        int col = 0;
        long long pkts = 0;
        int is_accept = 0, is_udp = 0, out_ok = 0, spt_ok = 0;
        for (tok = strtok_r(line, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
            if (col == 0) pkts = atoll(tok);
            else if (col == 2) is_accept = (strcmp(tok, "ACCEPT") == 0);
            else if (col == 3) is_udp = (strcmp(tok, "udp") == 0);
            else if (col == 6) out_ok = (strcmp(tok, nic) == 0);
            else if (col >= 8) {
                if (strstr(tok, "spt:123")) spt_ok = 1;
            }
            col++;
        }
        if (is_accept && is_udp && out_ok && spt_ok) { value = pkts; break; }
    }
    return value;
}

/* Capture each rule's counter as the baseline.  The iptables rules outlive
 * the daemon, so without this a restart would keep the old response total
 * while `req` restarted at zero -- and the response ratio would be garbage. */
static void ipt_baseline(void) {
    int i;
    long long v;
    if (!use_iptables) return;
    for (i = 0; i < g_nnic; i++) {
        v = ipt_out_count(g_nics[i].name);
        g_nics[i].ipt_rule = (v >= 0) ? (unsigned long long)v : 0;
    }
}

static void poll_responses(void) {
    int i;
    long long v;
    if (!use_iptables) return;
    for (i = 0; i < g_nnic; i++) {
        v = ipt_out_count(g_nics[i].name);
        if (v < 0) continue;
        {
            unsigned long long cur = (unsigned long long)v;
            /* per-minute response delta, so the window can be reported
             * alongside the request window instead of only cumulatively */
            if (g_nics[i].ipt_last && cur >= g_nics[i].ipt_last) {
                unsigned long long delta = cur - g_nics[i].ipt_last;
                if (delta) {
                    long m = (long)(time(NULL) / 60);
                    bucket_advance(&g_nics[i], m);
                    g_nics[i].rbucket[m % BUCKETS] += (unsigned int)delta;
                }
            }
            g_nics[i].ipt_last = cur;
            if (cur >= g_nics[i].ipt_rule)
                g_nics[i].rsp = cur - g_nics[i].ipt_rule;
        }
    }
}

/* ── JSON output ───────────────────────────────────────────────────── */

static int cmp_client(const void *a, const void *b) {
    const ClientEnt *x = a, *y = b;
    if (x->req != y->req) return x->req < y->req ? 1 : -1;
    return 0;
}

static void write_json(void) {
    static ClientEnt sorted[CLIENT_SLOTS];
    char tmp[256];
    FILE *f;
    int i, n = 0, first = 1, w;
    time_t now = time(NULL);
    long m = now / 60;

    snprintf(tmp, sizeof(tmp), "%s.tmp", out_path);
    f = fopen(tmp, "w");
    if (!f) { logmsg("cannot write %s: %s", tmp, strerror(errno)); return; }
    fchmod(fileno(f), 0644);

    fprintf(f, "{\"ts\":%ld,\"sec_t0\":%ld,\"nics\":[", (long)now, (long)now - SSEC + 1);
    for (i = 0; i < g_nnic; i++) {
        if (!g_nics[i].used) continue;
        /* Both windows are published: the cumulative totals (since daemon
           start) and the trailing-hour sums.  The UI shows the hour window so
           every column on the row shares one time base -- mixing a cumulative
           request total with an hour-scoped client list read as a
           contradiction ("138 requests / 0 clients"). */
        fprintf(f, "%s{\"name\":\"%s\",\"ifindex\":%d,"
                   "\"req\":%llu,\"valid\":%llu,\"rsp\":%llu,",
                first ? "" : ",", g_nics[i].name, g_nics[i].ifindex,
                g_nics[i].req, g_nics[i].valid, g_nics[i].rsp);
        /* one set of sums per offered range, from the same 24h rings */
        for (w = 0; w < NWINDOWS; w++) {
            int mins = g_windows[w] / 60;
            fprintf(f, "\"req_%s\":%u,\"valid_%s\":%u,\"rsp_%s\":%u,",
                    g_winsfx[w],
                    bucket_sum(g_nics[i].bucket,  g_nics[i].bucket_min, m, mins),
                    g_winsfx[w],
                    bucket_sum(g_nics[i].vbucket, g_nics[i].bucket_min, m, mins),
                    g_winsfx[w],
                    bucket_sum(g_nics[i].rbucket, g_nics[i].bucket_min, m, mins));
        }
        fprintf(f, "\"windows_s\":[");
        for (w = 0; w < NWINDOWS; w++)
            fprintf(f, "%s%d", w ? "," : "", g_windows[w]);
        fprintf(f, "],\"buckets\":[");
        {
            const unsigned int *rings[3];
            int r, k;
            rings[0] = g_nics[i].bucket;
            rings[1] = g_nics[i].vbucket;
            rings[2] = g_nics[i].rbucket;
            for (r = 0; r < 3; r++) {
                fprintf(f, "%s[", r ? "," : "");
                /* publish only the trailing hour: all 1440 would add ~60 KB
                   to a snapshot that is rewritten every few seconds, and
                   nothing consumes more than the recent tail. */
                for (k = BUCKETS - BUCKETS_OUT; k < BUCKETS; k++) {
                    long bm = m - (BUCKETS - 1) + k;   /* oldest -> newest */
                    unsigned int v = 0;
                    if (bm <= g_nics[i].bucket_min && g_nics[i].bucket_min - bm < BUCKETS)
                        v = rings[r][bm % BUCKETS];
                    fprintf(f, "%s%u", k > BUCKETS - BUCKETS_OUT ? "," : "", v);
                }
                fprintf(f, "]");
            }
        }
        fprintf(f, "],\"sec\":[");
        {
            /* trailing SSEC seconds, oldest -> newest, 0 for idle/gapped
             * seconds (the same masking bucket_sum applies to the minute
             * rings).  The UI's 1-minute view slices the trailing minute
             * out of this and plots the values as 条/秒 directly. */
            int k;
            for (k = 0; k < SSEC; k++) {
                long e = (long)now - (SSEC - 1) + k;
                unsigned int v = 0;
                if (g_nics[i].sreq_min > 0 && e <= g_nics[i].sreq_min &&
                    g_nics[i].sreq_min - e < SSEC)
                    v = g_nics[i].sreq[e % SSEC];
                fprintf(f, "%s%u", k ? "," : "", v);
            }
        }
        fprintf(f, "]}");
        first = 0;
    }
    fprintf(f, "],\"clients\":{");

    for (i = 0; i < CLIENT_SLOTS; i++)
        if (g_clients[i].used) sorted[n++] = g_clients[i];
    if (n > 1) qsort(sorted, (size_t)n, sizeof(sorted[0]), cmp_client);

    first = 1;
    {
        int ni, emitted;
        for (ni = 0; ni < g_nnic; ni++) {
            char ipbuf[INET_ADDRSTRLEN];
            struct in_addr in;
            emitted = 0;
            if (!g_nics[ni].used) continue;
            fprintf(f, "%s\"%s\":[", first ? "" : ",", g_nics[ni].name);
            for (i = 0; i < n && emitted < TOP_CLIENTS; i++) {
                if (sorted[i].ifindex != g_nics[ni].ifindex) continue;
                in.s_addr = sorted[i].ip;
                inet_ntop(AF_INET, &in, ipbuf, sizeof(ipbuf));
                fprintf(f, "%s{\"ip\":\"%s\",\"req\":%llu,\"valid\":%llu,\"last_age_s\":%ld}",
                        emitted ? "," : "", ipbuf, sorted[i].req, sorted[i].valid,
                        (long)(now - sorted[i].last));
                emitted++;
            }
            fprintf(f, "]");
            first = 0;
        }
    }
    fprintf(f, "},\"clients_total\":%d,\"rsp_source\":\"%s\"}",
            n, use_iptables ? "iptables" : "disabled");
    fclose(f);
    if (rename(tmp, out_path) < 0) logmsg("rename: %s", strerror(errno));
}

/* ── main ──────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    int opt, fd;
    struct sockaddr_ll sll;
    static const char *default_nics = "eth0,eth1,eth2,eth3";
    const char *nics = default_nics;
    time_t last_write = 0, last_expire = 0;
    struct sigaction sa;

    while ((opt = getopt(argc, argv, "f:i:n:CDh")) != -1) {
        switch (opt) {
        case 'f': out_path = optarg; break;
        case 'i': interval = atoi(optarg); if (interval < 1) interval = 1; break;
        case 'n': nics = optarg; break;
        case 'C': use_iptables = 0; break;
        case 'D': break;   /* foreground is the default here */
        default:
            fprintf(stderr, "usage: %s [-f out.json] [-i secs] [-n nic,nic,..] [-C]\n", argv[0]);
            return 2;
        }
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    {
        char *copy = strdup(nics), *tok, *save;
        for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            while (*tok == ' ') tok++;
            add_nic(tok);
        }
    }
    if (g_nnic == 0) { logmsg("no usable interfaces, exiting"); return 1; }

    fd = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
    if (fd < 0) {
        logmsg("AF_PACKET socket failed: %s (need CAP_NET_RAW/root)", strerror(errno));
        return 1;
    }
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_IP);
    sll.sll_ifindex = 0;                 /* all interfaces; ifindex comes back per packet */
    {   /* non-blocking: capture_drain() must return to the main loop */
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        logmsg("bind failed: %s", strerror(errno));
        close(fd);
        return 1;
    }
    logmsg("started: %d nic(s), out=%s, interval=%ds, rsp_source=%s",
           g_nnic, out_path, interval, use_iptables ? "iptables" : "disabled");

    if (use_iptables) {
        ipt_setup();
        ipt_baseline();     /* rsp is reported since this daemon start */
    }

    for (;;) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int pr;
        time_t now;
        if (stop_flag) break;
        pr = poll(&pfd, 1, 1000);
        if (pr > 0 && (pfd.revents & POLLIN)) capture_drain(fd);
        now = time(NULL);
        if (now - last_write >= interval) {
            last_write = now;
            if (use_iptables) poll_responses();
            write_json();
        }
        if (now - last_expire >= 60) {
            last_expire = now;
            clients_expire(now);
        }
    }

    close(fd);
    logmsg("exit");
    return 0;
}

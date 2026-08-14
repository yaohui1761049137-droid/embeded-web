/* test_remote.c — host-side unit tests for the remote module (offline)
 *
 * Drives remote.c against a scripted fake board over a pty pair
 * (test_fake_board.py), exercising every branch the real-board e2e
 * suite can never reach: retry, timeout, ERR payloads, garbled
 * responses.  Also asserts the exact command sequence (wire-change
 * guard) and the board lookup table.
 *
 * Usage: ./test_remote [fake_board.py]   (default test_fake_board.py)
 * Compile (host): gcc -Wall -O2 -o test_remote test_remote.c \
 *                 src/remote.c src/common.c
 */
#include "src/remote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#define FAKE_LOG "/tmp/remote_fake.log"

static int g_pass = 0, g_fail = 0;
static pid_t g_fake_pid = -1;
static char g_slave[128];

static void check(int cond, const char *desc) {
    if (cond) { g_pass++; printf("  ✅ %s\n", desc); }
    else      { g_fail++; printf("  ❌ %s\n", desc); }
}

static void expect_fail(int rc, const char *err, const char *needle,
                        const char *desc) {
    int ok = (rc != 0 && err && strstr(err, needle) != NULL);
    check(ok, desc);
    if (!ok) printf("      (rc=%d, err=\"%s\")\n", rc, err ? err : "(null)");
}

/* ── fake board process management ─────────────────────────────────── */

static int start_fake(const char *script, const char *scenario) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(pipefd[1], 1);
        close(pipefd[0]);
        execlp("python3", "python3", script, scenario, FAKE_LOG, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    char buf[128];
    int n = (int)read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    if (n <= 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    snprintf(g_slave, sizeof(g_slave), "%s", buf);
    g_fake_pid = pid;
    setenv("REMOTE_SERIAL_DEVICE_OVERRIDE", g_slave, 1);
    return 0;
}

static void stop_fake(void) {
    if (g_fake_pid > 0) {
        kill(g_fake_pid, SIGTERM);
        waitpid(g_fake_pid, NULL, 0);
        g_fake_pid = -1;
    }
}

/* log contains needle; returns -1 if absent */
static long log_pos(const char *needle) {
    FILE *f = fopen(FAKE_LOG, "r");
    if (!f) return -1;
    static char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    char *p = strstr(buf, needle);
    return p ? (long)(p - buf) : -1;
}

/* ── tests ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    const char *script = (argc > 1) ? argv[1] : "test_fake_board.py";
    char err[256];
    RemoteNetConfig cfg;
    RemoteBoard *h;

    /* ── board lookup (no fake needed) ───────────────────────────── */
    check(remote_board_lookup(NULL) == 0, "lookup: NULL port → Board 1");
    check(remote_board_lookup("") == 0,   "lookup: empty port → Board 1");
    check(remote_board_lookup("s4") == 1, "lookup: s4 → Board 2");
    check(remote_board_lookup("zz") == -1, "lookup: unknown port rejected");
    check(remote_board_open(-1) == NULL,  "open: invalid board refused");
    check(remote_board_open(99) == NULL,  "open: out-of-range board refused");

    /* ── scenario: ok ────────────────────────────────────────────── */
    check(start_fake(script, "ok") == 0, "fake ok: pty started");
    h = remote_board_open(0);
    check(h != NULL, "open: pty device (PING warmup exchanged)");
    memset(&cfg, 0, sizeof(cfg));
    check(remote_query(h, &cfg, err, sizeof(err)) == 0,
          "query: OK on both commands");
    check(strcmp(cfg.ip, "10.0.0.1") == 0 &&
          strcmp(cfg.mask, "255.255.255.0") == 0 &&
          strcmp(cfg.gateway, "10.0.0.254") == 0,
          "query: ipv4 parsed from 'OK ip mask gw'");
    check(strcmp(cfg.ipv6, "fd00::1/64") == 0,
          "query: ipv6 parsed from 'OK addr/prefix'");
    long p_ping = log_pos("PING"), p_get = log_pos("REMOTE_GET_IPV4");
    check(p_ping >= 0 && p_get > p_ping,
          "wire: PING warmup precedes REMOTE_GET_IPV4");
    remote_board_close(h);
    stop_fake();

    /* ── scenario: no_ping (warmup result discarded) ─────────────── */
    check(start_fake(script, "no_ping") == 0, "fake no_ping: pty started");
    h = remote_board_open(0);
    check(h != NULL, "open: succeeds even when PING is unanswered");
    memset(&cfg, 0, sizeof(cfg));
    check(remote_query(h, &cfg, err, sizeof(err)) == 0 &&
          strcmp(cfg.ip, "10.0.0.1") == 0,
          "query: data commands still served");
    remote_board_close(h);
    stop_fake();

    /* ── scenario: retry (first data command unanswered) ─────────── */
    check(start_fake(script, "retry") == 0, "fake retry: pty started");
    h = remote_board_open(0);
    memset(&cfg, 0, sizeof(cfg));
    check(remote_query(h, &cfg, err, sizeof(err)) == 0 &&
          strcmp(cfg.ip, "10.0.0.1") == 0,
          "query: recovers after one unanswered attempt");
    remote_board_close(h);
    stop_fake();

    /* ── scenario: garbled ───────────────────────────────────────── */
    check(start_fake(script, "garbled") == 0, "fake garbled: pty started");
    h = remote_board_open(0);
    memset(&cfg, 0, sizeof(cfg));
    expect_fail(remote_query(h, &cfg, err, sizeof(err)), err,
                "Garbled response after 3 retries",
                "query: garbled responses exhaust retries");
    check(cfg.ip[0] == '\0' && cfg.ipv6[0] == '\0',
          "query: garbled board leaves fields empty");
    remote_board_close(h);
    stop_fake();

    /* ── scenario: silent ────────────────────────────────────────── */
    check(start_fake(script, "silent") == 0, "fake silent: pty started");
    h = remote_board_open(0);
    memset(&cfg, 0, sizeof(cfg));
    expect_fail(remote_query(h, &cfg, err, sizeof(err)), err,
                "Remote Board not responding",
                "query: silent board times out");
    check(cfg.ip[0] == '\0' && cfg.ipv6[0] == '\0',
          "query: silent board leaves fields empty");
    remote_board_close(h);
    stop_fake();

    /* ── configure: success + ERR payloads ───────────────────────── */
    check(start_fake(script, "ok") == 0, "fake ok #2: pty started");
    h = remote_board_open(0);
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.ip, "192.168.8.99");
    strcpy(cfg.mask, "255.255.255.0");
    strcpy(cfg.gateway, "192.168.8.1");
    check(remote_configure(h, &cfg, err, sizeof(err)) == 0,
          "configure: OK (no ipv6 → no REMOTE_SET_IPV6)");
    check(log_pos("REMOTE_SET_IPV6") < 0,
          "wire: ipv6 empty → SET_IPV6 not sent");
    remote_board_close(h);
    stop_fake();

    check(start_fake(script, "err") == 0, "fake err: pty started");
    h = remote_board_open(0);
    strcpy(cfg.ipv6, "fd00::99/64");
    expect_fail(remote_configure(h, &cfg, err, sizeof(err)), err,
                "IPv4: Invalid address",
                "configure: IPv4 ERR payload composed as 'IPv4: <msg>'");
    remote_board_close(h);
    stop_fake();

    check(start_fake(script, "err6") == 0, "fake err6: pty started");
    h = remote_board_open(0);
    expect_fail(remote_configure(h, &cfg, err, sizeof(err)), err,
                "IPv6: Invalid prefix",
                "configure: IPv6-only failure gets a real message "
                "(original CGI printed an empty one)");
    remote_board_close(h);
    stop_fake();

    printf("\n Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

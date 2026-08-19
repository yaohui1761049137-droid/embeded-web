/* nmcli.c — Local NIC configuration via NetworkManager (ADR-0003)
 *
 * All nmcli invocations go through nmcli_run(): fork/execvp (never a
 * shell), so parameter validation here is defense-in-depth rather than
 * the only line of defense.  NMCLI_OVERRIDE swaps the executable for
 * offline host testing (fake script).
 */
#include "nmcli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#define NMCLI_MAX_NICS 4

static const char *g_nics[NMCLI_MAX_NICS] = { "eth0", "eth1", "eth2", "eth3" };

/* ── Command execution (no shell) ─────────────────────────────────── */

static int nmcli_run(char *const argv[], char *out, size_t outlen,
                     char *err, size_t errlen) {
    /* Board: the CGI runs as www-data, so nmcli goes through the
     * sudoers whitelist (ADR-0003 D9).  Host tests override the binary
     * (NMCLI_OVERRIDE) or set NMCLI_NO_SUDO=1. */
    const char *exe = getenv("NMCLI_OVERRIDE");
    int use_sudo = 0;
    if (!exe || !*exe) {
        exe = "nmcli";
        if (!getenv("NMCLI_NO_SUDO")) use_sudo = 1;
    }

    int argc = 0;
    while (argv[argc]) argc++;
    if (argc + 3 > 64) {
        snprintf(err, errlen, "too many arguments");
        return -1;
    }
    char *exec_argv[64];
    int idx = 0;
    if (use_sudo) {
        exec_argv[idx++] = "sudo";
        exec_argv[idx++] = "-n";
    }
    exec_argv[idx++] = (char *)exe;
    int i;
    for (i = 1; i < argc; i++) exec_argv[idx++] = argv[i];
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
        /* exec argv[0], not exe: on the board exec_argv[0] is "sudo"
         * (exe is the command sudo will run), on host tests with
         * NMCLI_OVERRIDE both are the fake's path.  Passing exe here
         * would exec nmcli itself with argv[0]="sudo". */
        execvp(exec_argv[0], exec_argv);
        _exit(127);
    }
    close(pipefd[1]);

    size_t pos = 0;
    ssize_t n;
    char tmp[512];
    /* capture up to outlen-1 bytes, then drain the rest before waitpid:
     * closing the read end first would SIGPIPE-kill a child that writes
     * after we close (e.g. fire-and-forget calls with out == NULL) */
    while (out && pos + 1 < outlen &&
           (n = read(pipefd[0], out + pos, outlen - pos - 1)) > 0)
        pos += (size_t)n;
    if (out && outlen) out[pos] = '\0';
    while (read(pipefd[0], tmp, sizeof(tmp)) > 0) { }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        /* out may be NULL for fire-and-forget calls */
        const char *outinfo = (out && outlen) ? out : "";
        snprintf(err, errlen, "nmcli %s %s failed: %s", argv[1], argv[2], outinfo);
        return -1;
    }
    return 0;
}

/* ── Validation ───────────────────────────────────────────────────── */

int nmcli_nic_valid(const char *nic) {
    int i;
    for (i = 0; i < NMCLI_MAX_NICS; i++)
        if (strcmp(g_nics[i], nic) == 0) return 1;
    return 0;
}

static int valid_ipv4(const char *s) {
    if (!s || !*s) return 0;
    int seg = 0, dots = 0;
    const char *p = s;
    for (; *p; p++) {
        if (*p == '.') {
            if (p == s || p[-1] == '.' || seg > 255 || ++dots > 3) return 0;
            seg = 0;
        } else if (*p >= '0' && *p <= '9') {
            seg = seg * 10 + (*p - '0');
            if (seg > 255) return 0;
        } else {
            return 0;
        }
    }
    return dots >= 1 && seg <= 255 && p[-1] >= '0' && p[-1] <= '9';
}

static int valid_mask(const char *s) {
    /* like valid_ipv4, plus: the 32-bit mask's 1-bits must be
     * contiguous from the MSB (NM derives the mask from the prefix,
     * so a non-contiguous mask would silently change value) */
    if (!valid_ipv4(s)) return 0;
    int v[4] = {0, 0, 0, 0}, seg = 0, i = 0;
    const char *p;
    for (p = s; *p; p++) {
        if (*p == '.') { v[i++] = seg; seg = 0; }
        else           { seg = seg * 10 + (*p - '0'); }
    }
    v[i] = seg;
    unsigned int m = 0;
    for (i = 0; i < 4; i++) m = (m << 8) | (v[i] & 0xff);
    int seen_zero = 0;
    for (i = 31; i >= 0; i--) {
        if (m & (1u << i)) {
            if (seen_zero) return 0;
        } else {
            seen_zero = 1;
        }
    }
    return 1;
}

static int valid_dns_list(const char *s) {
    /* comma-separated IPv4 addresses */
    if (!s || !*s) return 0;
    char tmp[192];
    snprintf(tmp, sizeof(tmp), "%s", s);
    char *tok = strtok(tmp, ",");
    while (tok) {
        if (!valid_ipv4(tok)) return 0;
        tok = strtok(NULL, ",");
    }
    return 1;
}

static int valid_ipv6(const char *s) {
    /* lenient shape check: addr/prefix, hex/colon/dot chars only */
    if (!s || !*s) return 0;
    if (strchr(s, ':') == NULL) return 0;
    size_t len = strlen(s);
    if (len > 120) return 0;
    size_t i;
    for (i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F') || c == ':' || c == '.' || c == '/'))
            return 0;
    }
    const char *slash = strchr(s, '/');
    if (!slash || !*(slash + 1)) return 0;
    int prefix = atoi(slash + 1);
    if (prefix < 1 || prefix > 128) return 0;
    const char *p;
    for (p = slash + 1; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

int nmcli_validate_params(const NicConfig *cfg, char *err, size_t errlen) {
    if (!cfg->ip || !*cfg->ip || !valid_ipv4(cfg->ip)) {
        snprintf(err, errlen, "IP 地址格式无效");
        return 0;
    }
    if (!cfg->mask || !*cfg->mask || !valid_mask(cfg->mask)) {
        snprintf(err, errlen, "子网掩码格式无效");
        return 0;
    }
    if (cfg->gateway && *cfg->gateway && !valid_ipv4(cfg->gateway)) {
        snprintf(err, errlen, "网关地址格式无效");
        return 0;
    }
    if (cfg->dns && *cfg->dns && !valid_dns_list(cfg->dns)) {
        snprintf(err, errlen, "DNS 服务器格式无效");
        return 0;
    }
    if (cfg->ipv6 && *cfg->ipv6 && !valid_ipv6(cfg->ipv6)) {
        snprintf(err, errlen, "IPv6 地址格式无效");
        return 0;
    }
    return 1;
}

/* ── Mask ⇄ prefix ────────────────────────────────────────────────── */

int nmcli_mask_to_prefix(const char *mask) {
    /* parse the 4 octets as decimal, then count leading 1-bits
     * (valid_mask guarantees the 1-bits are contiguous) */
    int v[4] = {0, 0, 0, 0}, seg = 0, i = 0;
    const char *p;
    for (p = mask; *p; p++) {
        if (*p == '.') { v[i++] = seg; seg = 0; }
        else           { seg = seg * 10 + (*p - '0'); }
    }
    v[i] = seg;
    unsigned int m = 0;
    for (i = 0; i < 4; i++) m = (m << 8) | (v[i] & 0xff);
    int ones = 0;
    while (m & 0x80000000u) { ones++; m <<= 1; }
    return ones;
}

static void prefix_to_mask(int prefix, char *out, size_t outlen) {
    unsigned int m = prefix == 0 ? 0 : 0xffffffffu << (32 - prefix);
    snprintf(out, outlen, "%u.%u.%u.%u",
             (m >> 24) & 0xff, (m >> 16) & 0xff, (m >> 8) & 0xff, m & 0xff);
}

/* ── Profile lookup ───────────────────────────────────────────────── */

static char *profile_for(const char *nic, char *err, size_t errlen) {
    /* Find the profile bound to nic: the list view cannot filter by
     * interface-name and its DEVICE column only reflects inactive
     * profiles unreliably, so scan each connection's full output for
     * "connection.interface-name:<nic>".  Returns the UUID — hex-only,
     * no spaces (unlike names like "Wired connection 1"), so it is
     * stable and matches the sudoers whitelist wildcards. */
    char out[8192];
    char *argv[] = { "nmcli", "-t", "-f", "NAME,UUID", "connection",
                     "show", NULL };
    if (nmcli_run(argv, out, sizeof(out), err, errlen) < 0) return NULL;

    char *line = out;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            char showout[8192];
            char *argv2[] = { "nmcli", "-t", "connection", "show", line, NULL };
            if (nmcli_run(argv2, showout, sizeof(showout), err, errlen) == 0) {
                char *cl = showout;
                while (cl && *cl) {
                    char *cnl = strchr(cl, '\n');
                    if (cnl) *cnl = '\0';
                    if (strncmp(cl, "connection.interface-name:", 26) == 0 &&
                        strcmp(cl + 26, nic) == 0) {
                        char *ret = strdup(colon + 1);
                        if (nl) *nl = '\n';
                        if (cnl) *cnl = '\n';
                        return ret;
                    }
                    if (cnl) *cnl = '\n';
                    cl = cnl ? cnl + 1 : NULL;
                }
            }
            *colon = ':';
        }
        if (nl) { *nl = '\n'; }
        line = nl ? nl + 1 : NULL;
    }
    snprintf(err, errlen, "未找到网口 %s 的 NetworkManager 配置", nic);
    return NULL;
}

/* ── Query ────────────────────────────────────────────────────────── */

int nmcli_get_config(const char *nic, NicConfig *out, char *err, size_t errlen) {
    if (!nmcli_nic_valid(nic)) {
        snprintf(err, errlen, "无效的网口名称");
        return -1;
    }
    memset(out, 0, sizeof(*out));

    char buf[8192];
    char *argv[] = { "nmcli", "-t", "device", "show", (char *)nic, NULL };
    if (nmcli_run(argv, buf, sizeof(buf), err, errlen) < 0) return -1;

    /* parse "KEY:value" lines */
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            const char *key = line;
            const char *val = colon + 1;
            if (strncmp(key, "IP4.ADDRESS[1]", 14) == 0) {
                char addr[64];
                snprintf(addr, sizeof(addr), "%s", val);
                char *slash = strchr(addr, '/');
                if (slash) {
                    *slash = '\0';
                    snprintf(out->ip, sizeof(out->ip), "%s", addr);
                    prefix_to_mask(atoi(slash + 1), out->mask, sizeof(out->mask));
                } else {
                    snprintf(out->ip, sizeof(out->ip), "%s", addr);
                }
            } else if (strncmp(key, "IP4.GATEWAY", 11) == 0) {
                snprintf(out->gateway, sizeof(out->gateway), "%s", val);
            } else if (strncmp(key, "IP4.DNS[", 8) == 0) {
                if (*out->dns)
                    strncat(out->dns, ",", sizeof(out->dns) - strlen(out->dns) - 1);
                strncat(out->dns, val,
                        sizeof(out->dns) - strlen(out->dns) - 1);
            } else if (strncmp(key, "IP6.ADDRESS", 11) == 0) {
                /* skip link-local (fe80::) — always present on a live
                 * NIC; report the first global address instead */
                if (strncmp(val, "fe80:", 5) != 0 && !*out->ipv6)
                    snprintf(out->ipv6, sizeof(out->ipv6), "%s", val);
            }
            *colon = ':';
        }
        if (nl) { *nl = '\n'; }
        line = nl ? nl + 1 : NULL;
    }
    return 0;
}

/* ── Set ──────────────────────────────────────────────────────────── */

static void clear_gateway_others(const char *keep_nic, char *err, size_t errlen) {
    int i;
    for (i = 0; i < NMCLI_MAX_NICS; i++) {
        const char *nic = g_nics[i];
        if (strcmp(nic, keep_nic) == 0) continue;
        NicConfig cur;
        if (nmcli_get_config(nic, &cur, err, errlen) < 0) continue;
        if (!*cur.gateway) continue;
        char *profile = profile_for(nic, err, errlen);
        if (!profile) continue;
        char *argv[] = { "nmcli", "connection", "modify", profile,
                         "ipv4.gateway", "", NULL };
        nmcli_run(argv, NULL, 0, err, errlen);
        free(profile);
    }
}

int nmcli_set_config(const char *nic, const NicConfig *cfg, char *err, size_t errlen) {
    if (!nmcli_nic_valid(nic)) {
        snprintf(err, errlen, "无效的网口名称");
        return -1;
    }
    char *profile = profile_for(nic, err, errlen);
    if (!profile) {
        /* Fresh NIC (eth1-3 have no bound profile): create a persistent
         * one named after the NIC — the modify below then applies, and
         * it re-applies on link-up. */
        char *argv[] = { "nmcli", "connection", "add", "type", "ethernet",
                         "ifname", (char *)nic, "con-name", (char *)nic, NULL };
        if (nmcli_run(argv, NULL, 0, err, errlen) < 0) return -1;
        profile = profile_for(nic, err, errlen);
        if (!profile) return -1;
    }

    /* ipv4.addresses wants CIDR; mask → prefix */
    int prefix = nmcli_mask_to_prefix(cfg->mask);
    char cidr[64];
    snprintf(cidr, sizeof(cidr), "%s/%d", cfg->ip, prefix);

    char *argv[32];
    int n = 0;
    argv[n++] = "nmcli";
    argv[n++] = "connection";
    argv[n++] = "modify";
    argv[n++] = profile;
    argv[n++] = "ipv4.method";
    argv[n++] = "manual";
    argv[n++] = "ipv4.addresses";
    argv[n++] = cidr;
    argv[n++] = "ipv4.gateway";
    argv[n++] = (char *)((cfg->gateway && *cfg->gateway) ? cfg->gateway : "");
    argv[n++] = "ipv4.dns";
    argv[n++] = (char *)((cfg->dns && *cfg->dns) ? cfg->dns : "");
    argv[n++] = "ipv6.method";
    if (cfg->ipv6 && *cfg->ipv6) {
        argv[n++] = "manual";
        argv[n++] = "ipv6.addresses";
        argv[n++] = (char *)cfg->ipv6;
    } else {
        /* "ignore" not "disabled": the board's NetworkManager 1.14
         * rejects "disabled" (added in NM 1.42) */
        argv[n++] = "ignore";
    }
    argv[n] = NULL;

    if (nmcli_run(argv, NULL, 0, err, errlen) < 0) {
        free(profile);
        return -1;
    }

    /* unique gateway: only the newly configured NIC may carry one */
    if (cfg->gateway && *cfg->gateway)
        clear_gateway_others(nic, err, errlen);

    /* bring the profile up; a down/NO-CARRIER NIC fails here and that
     * is fine — the profile is saved and applies on link-up. */
    char *argv2[] = { "nmcli", "connection", "up", profile, NULL };
    nmcli_run(argv2, NULL, 0, err, errlen);

    free(profile);
    return 0;
}

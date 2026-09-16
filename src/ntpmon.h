/* ntpmon.h — NTP access control (chrony allow/deny) + traffic stats
 * for the web「服务监控及报警」tab.
 *
 * ACL: the managed rule set lives in /etc/chrony/acl-web.conf (included
 * from chrony.conf; migrated from the pre-existing allow lines at
 * deploy time).  Rule changes go through chrony_acl_apply.sh via sudo
 * (www-data NOPASSWD whitelist): additions use `chronyc allow|deny`
 * (instant, no restart) and persist to the file; removals rewrite the
 * file and restart chronyd — chrony 3.4 has no runtime remove/list
 * command, and SIGHUP is a quit signal there, never used.
 *
 * Stats: /var/db/ntp_stats.csv, one row per board-minute, written by
 * ntp_stats_sample.sh:
 *   epoch,ntp_hits,ntp_drops,cmd_hits,cmd_drops,log_drops,e0,e1,e2,e3
 * (per-interface columns come from count-only iptables rules, udp/123.)
 *
 * Host tests override: NTPMON_ACL_FILE, NTPMON_CSV,
 * NTPMON_HELPER_OVERRIDE (helper executable).
 */
#ifndef NTPMON_H
#define NTPMON_H

#include <stddef.h>

#define NTPMON_MAX_RULES 64
#define NTPMON_MAX_ROWS  1441   /* 24 h at 1 sample/min, +1 for the first delta */

typedef struct {
    char action[8];   /* "allow" | "deny" */
    char cidr[64];
} NtpAclRule;

typedef struct {
    long long t;
    long long hits, drops, cmd_hits, cmd_drops, log_drops;   /* chrony */
    long long eth[4];                                        /* iptables */
} NtpMonRow;

/* Shape validation for a CIDR (IPv4/IPv6 with optional /prefix).
 * The authoritative check is chronyc itself on the board. 1 ok, 0 bad. */
int ntpmon_cidr_valid(const char *cidr);

/* Load managed rules from the ACL file.
 * Returns the rule count, or -1 if the file cannot be read. */
int ntpmon_acl_load(NtpAclRule *out, int max);

/* Find a rule by CIDR (either action).  Returns index or -1. */
int ntpmon_acl_find(const NtpAclRule *rules, int n, const char *cidr);

/* Apply a rule change through the helper (sudo -n on the board).
 * op = "add" (uses action) or "remove".  Returns 0 on success, -1 with
 * err set (includes the helper's output on failure). */
int ntpmon_acl_apply(const char *op, const char *action, const char *cidr,
                     char *err, size_t errlen);

/* Build the stats JSON document (acl rules, 24 h series, per-interface
 * bars, meta).  Returns 0 on success, -1 if out is too small. */
int ntpmon_stats_json(char *out, size_t outlen);

#endif /* NTPMON_H */

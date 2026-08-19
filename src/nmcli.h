/* nmcli.h — Local NIC configuration via NetworkManager (nmcli)
 *
 * Replaces the Remote Board serial protocol (ADR-0003): the 4 NICs
 * eth0-3 are configured locally through NetworkManager, whose
 * connection profiles are the single source of truth (no config file
 * layer).  network.cgi.c only validates params and calls these
 * functions; profile lookup, gateway migration and command escaping
 * live here.
 *
 * On the board the CGI runs as www-data and reaches nmcli through the
 * sudoers whitelist (sudo -n).  Offline testing: NMCLI_OVERRIDE swaps
 * the executable (like gate's DB_PATH), NMCLI_NO_SUDO=1 skips sudo for
 * host runs without an override.
 */
#ifndef NMCLI_H
#define NMCLI_H

#include <stddef.h>

typedef struct {
    char ip[64];      /* dotted quad, e.g. "192.168.8.201" ("" if none) */
    char mask[64];    /* dotted quad, e.g. "255.255.255.0" */
    char gateway[64]; /* dotted quad ("" if none) */
    char dns[192];    /* comma-separated server list ("" if none) */
    char ipv6[128];   /* "addr/prefix", e.g. "2001:db8::1/64" ("" if none) */
} NicConfig;

/* 1 if nic is one of the managed NICs (eth0..eth3), 0 otherwise. */
int nmcli_nic_valid(const char *nic);

/* Convert a dotted-quad mask to prefix length (255.255.255.0 → 24).
 * Only valid for contiguous masks (see nmcli_validate_params). */
int nmcli_mask_to_prefix(const char *mask);

/* Validate the field values of cfg (shapes only — NM still enforces
 * semantics).  err (may be NULL) gets a Chinese reason on failure.
 * Returns 1 if valid, 0 if not. */
int nmcli_validate_params(const NicConfig *cfg, char *err, size_t errlen);

/* Query the NIC's current effective config (device show).
 * Returns 0 on success, -1 on error with err set. */
int nmcli_get_config(const char *nic, NicConfig *out, char *err, size_t errlen);

/* Apply cfg to a NIC via its NM profile, then bring the profile up.
 * Unique-gateway migration (ADR-0003): when cfg->gateway is set, any
 * gateway currently configured on the *other* NICs is cleared first.
 * Returns 0 on success, -1 on error with err set. */
int nmcli_set_config(const char *nic, const NicConfig *cfg, char *err, size_t errlen);

#endif /* NMCLI_H */

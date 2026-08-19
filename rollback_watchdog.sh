#!/bin/sh
# rollback_watchdog.sh — eth0 rollback watchdog (ADR-0003 D10)
#
# Two entry points:
#   - systemd-run --on-active=180 (armed by network.cgi after an eth0
#     change; runs as root) — rolls back if no login signal appeared.
#   - rollback-recover.service at boot — recovers from a power loss
#     while a pending change was on disk.
#
# Login signals that keep the new config:
#   - web login: network.cgi wrote confirmed=1 via login.cgi
#   - SSH login: auth.log has an sshd Accepted line newer than ts
#     (the board is only reachable again if the new config works)
#
# State file /var/db/rollback.json is key=value lines, written by
# network.cgi, edited atomically by login.cgi.

JSON=${ROLLBACK_FILE:-/var/db/rollback.json}
[ -f "$JSON" ] || exit 0

nic=$(sed -n 's/^nic=//p' "$JSON")
ts=$(sed -n 's/^ts=//p' "$JSON")
confirmed=$(sed -n 's/^confirmed=//p' "$JSON")

keep() { rm -f "$JSON"; exit 0; }

if [ "$confirmed" = "1" ]; then keep; fi

# SSH signal: any Accepted after ts.  grep -a because auth.log on this
# board contains binary bytes (e.g. injected console noise).
AUTHLOG=${AUTHLOG:-/var/log/auth.log}
if [ -n "$ts" ] && [ -r "$AUTHLOG" ]; then
    last=$(grep -a 'sshd\[[0-9]*\]: Accepted ' "$AUTHLOG" | tail -1 |
           sed 's/^\([A-Z][a-z][a-z] *[0-9]* [0-9:]*\).*/\1/')
    if [ -n "$last" ]; then
        last_epoch=$(date -d "$last" +%s 2>/dev/null || echo 0)
        if [ "$last_epoch" -gt "$ts" ] 2>/dev/null; then keep; fi
    fi
fi

# No login signal within the window (or at boot): restore the old config.
[ -n "$nic" ] || exit 0
cidr=$(sed -n 's/^old_cidr=//p' "$JSON")
gw=$(sed -n 's/^old_gateway=//p' "$JSON")
dns=$(sed -n 's/^old_dns=//p' "$JSON")
v6=$(sed -n 's/^old_ipv6=//p' "$JSON")

# Profile lookup by UUID (names like "Wired connection 1" contain
# spaces, which break naive word-splitting here).
LIST=$(mktemp)
nmcli -t -f NAME,UUID connection show > "$LIST" 2>/dev/null
UUID=""
while IFS= read -r line; do
    name=${line%%:*}
    uuid=${line#*:}
    ifname=$(nmcli -t connection show "$name" | sed -n 's/^connection.interface-name://p')
    if [ "$ifname" = "$nic" ]; then UUID="$uuid"; break; fi
done < "$LIST"
rm -f "$LIST"
[ -n "$UUID" ] || exit 0

nmcli connection modify "$UUID" ipv4.method manual ipv4.addresses "$cidr" \
    ipv4.gateway "$gw" ipv4.dns "$dns" || exit 1
if [ -n "$v6" ]; then
    nmcli connection modify "$UUID" ipv6.method manual ipv6.addresses "$v6" || exit 1
else
    nmcli connection modify "$UUID" ipv6.method ignore || exit 1
fi
nmcli connection up "$UUID" >/dev/null 2>&1 || true

rm -f "$JSON"

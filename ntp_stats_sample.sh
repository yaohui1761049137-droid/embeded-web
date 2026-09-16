#!/bin/sh
# ntp_stats_sample.sh — one-shot NTP statistics sampler (systemd timer,
# every minute).  Feeds the web「服务监控及报警」tab via /var/db/ntp_stats.csv.
#
# Data sources:
#  - per-interface NTP request counters: count-only iptables rules
#    (udp/123 per ethN; ACCEPT matches the current default policy, so no
#    behaviour change).  Rules are (re)created idempotently here, which
#    also restores them after a reboot.
#  - chrony global counters: `chronyc -c serverstats`
#    (ntp_hits, ntp_drops, cmd_hits, cmd_drops, log_drops; all reset on
#    chronyd restart — the web chart handles the discontinuities).
#
# CSV row: epoch,ntp_hits,ntp_drops,cmd_hits,cmd_drops,log_drops,e0,e1,e2,e3
CSV=/var/db/ntp_stats.csv
KEEP=10080          # 7 days at 1 sample/min

for i in 0 1 2 3; do
    iptables -C INPUT -i eth$i -p udp --dport 123 -j ACCEPT 2>/dev/null || \
        iptables -I INPUT -i eth$i -p udp --dport 123 -j ACCEPT 2>/dev/null
done

SS=$(chronyc -c serverstats 2>/dev/null | tr -d '\r')
[ -n "$SS" ] || SS="0,0,0,0,0"

counts=
for i in 0 1 2 3; do
    n=$(iptables -nvx -L INPUT 2>/dev/null | awk -v ifn="eth$i" \
        '$3=="ACCEPT" && $4=="udp" && $6==ifn && /udp dpt:123/ {print $1; exit}')
    [ -n "$n" ] || n=0
    counts="$counts,$n"
done

echo "$(date +%s),$SS$counts" >> "$CSV"

# retention: keep ~7 days
lines=$(wc -l < "$CSV")
if [ "$lines" -gt $((KEEP * 2)) ]; then
    tail -n "$KEEP" "$CSV" > "$CSV.tmp" && mv "$CSV.tmp" "$CSV"
fi

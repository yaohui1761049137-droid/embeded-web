#!/bin/bash
# Deploy pps_tod with $GxGGA fix/satellite parsing + swap the binary.
# Rollback: /usr/local/bin/pps_tod.bak-pre-gga (previous verified binary).
set -e
echo '== backup current binary (keep the original verified rollback) =='
[ -f /usr/local/bin/pps_tod.bak-pre-gga ] || cp -f /usr/local/bin/pps_tod /usr/local/bin/pps_tod.bak-pre-gga
echo '== compile (no pipes — SIGPIPE lesson) =='
gcc -O2 -Wall -o /tmp/pps_tod_gga /tmp/pps_tod_gga.c -lpthread
ls -l /tmp/pps_tod_gga
echo '== swap =='
systemctl stop pps_tod pps_tod_watchdog
sleep 1
cp /tmp/pps_tod_gga /usr/local/bin/pps_tod
chmod 755 /usr/local/bin/pps_tod
systemctl start pps_tod
systemctl start pps_tod_watchdog
sleep 4
echo '== state =='
systemctl is-active pps_tod pps_tod_watchdog chrony
echo '== status file =='
cat /run/pps_tod/status
echo '== chronyc =='
chronyc sources | tail -2
echo PPS-TOD-GGA-DEPLOYED

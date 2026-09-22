#!/bin/bash
# Phase 1 of the Plan-A closed-loop test: isolated fast watchdog instance
# with an ABSENT status file (daemon-dead simulation).  After grace+window it
# should restart pps_tod (harmless, real daemon) -> RECOVERING + NTP disabled
# -> OBSERVE_GRACE passes -> DEGRADED (NTP serving stays disabled).
set -e
mkdir -p /tmp/nmtest
cat > /tmp/nmtest/test.conf <<'EOF'
GOOD_WINDOW=5
RESTART_COOLDOWN=5
MAX_RESTARTS=60
OBSERVE_GRACE=8
ALERT_INTERVAL=5
START_GRACE=3
STALE_MAX=5
NTP_GUARD=1
EOF
rm -f /tmp/nmtest/status /tmp/nmtest/state /tmp/nmtest/wd.log
PPS_TOD_STATUS=/tmp/nmtest/status \
PPS_TOD_STATE=/tmp/nmtest/state \
PPS_TOD_WATCHDOG_LOG=/tmp/nmtest/wd.log \
PPS_TOD_WATCHDOG_CONF=/tmp/nmtest/test.conf \
  setsid sh /usr/local/bin/pps_tod_watchdog.sh >/tmp/nmtest/stdout.log 2>&1 &
echo $! > /tmp/nmtest/pid
sleep 22
echo '=== t+22s: test watchdog log ==='
cat /tmp/nmtest/wd.log
echo '=== test state ==='
grep ntp_serving /tmp/nmtest/state || echo '(state not yet)'
echo '=== real watchdog untouched (should be OK) ==='
grep ntp_serving /run/pps_tod/watchdog.state
echo PHASE1-DONE

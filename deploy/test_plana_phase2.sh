#!/bin/bash
# Phase 2: feed a GOOD fake status into the test instance's path -> it should
# log "recovered" + re-enable NTP serving; then kill the test instance.
set -e
for i in $(seq 1 6); do
  printf 'ts=fake\ngood=1\nlast_good_age_s=0\noffset_us=5\n' > /tmp/nmtest/status.tmp
  mv /tmp/nmtest/status.tmp /tmp/nmtest/status
  sleep 1
done
sleep 12          # allow the 10s retry-throttle window to elapse
echo '=== test watchdog log ==='
tail -n 5 /tmp/nmtest/wd.log
echo '=== test state ==='
grep ntp_serving /tmp/nmtest/state || true
kill "$(cat /tmp/nmtest/pid)" 2>/dev/null || true
sleep 1
rm -rf /tmp/nmtest
echo '=== real watchdog after test ==='
grep ntp_serving /run/pps_tod/watchdog.state
systemctl is-active pps_tod pps_tod_watchdog chrony
echo PHASE2-DONE

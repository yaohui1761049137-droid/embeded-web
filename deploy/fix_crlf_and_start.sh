#!/bin/bash
# Strip CR from timing-stack files (Windows CRLF sources) and restart services.
set -e
for f in \
  /usr/local/bin/pps_tod_watchdog.sh \
  /usr/local/bin/pps_tod_rtc_save.sh \
  /usr/local/bin/sanitize-drift.sh \
  /etc/pps_tod/pps_tod_watchdog.conf \
  /etc/systemd/system/pps_tod_watchdog.service \
  /etc/systemd/system/pps_tod_rtc.service \
  /etc/systemd/system/pps_tod_rtc.timer \
  /etc/systemd/system/sanitize-drift.service \
  /etc/systemd/system/pps_tod.service \
  /etc/systemd/system/chrony.service.d/restart.conf ; do
  [ -f "$f" ] && sed -i 's/\r$//' "$f"
done
head -1 /usr/local/bin/sanitize-drift.sh | od -c | head -1
systemctl daemon-reload
systemctl restart sanitize-drift.service pps_tod.service
systemctl enable --now pps_tod_watchdog.service
sleep 3
echo '== state =='
echo "active:  $(systemctl is-active pps_tod) $(systemctl is-active pps_tod_watchdog) $(systemctl is-active sanitize-drift) $(systemctl is-active chrony)"
echo "enabled: $(systemctl is-enabled pps_tod) $(systemctl is-enabled pps_tod_watchdog) $(systemctl is-enabled pps_tod_rtc.timer) $(systemctl is-enabled sanitize-drift)"
ls -l /run/pps_tod/ 2>&1
echo '== pps_tod log tail =='
tail -n 4 /var/log/pps_tod/pps_tod_*.log 2>&1 | head -10
echo '== chronyc sources =='
chronyc sources 2>&1
echo TIMING-STACK-RUNNING

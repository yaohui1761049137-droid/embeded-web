#!/bin/bash
# Deploy the Plan-A watchdog (NTP serving guard) to the board.
set -e
sed -i 's/\r$//' /tmp/wd_watchdog.sh /tmp/wd_conf.conf
sh -n /tmp/wd_watchdog.sh && echo WATCHDOG-SYNTAX-OK
cp /tmp/wd_watchdog.sh /usr/local/bin/pps_tod_watchdog.sh
chmod 755 /usr/local/bin/pps_tod_watchdog.sh
cp /tmp/wd_conf.conf /etc/pps_tod/pps_tod_watchdog.conf
chmod 644 /etc/pps_tod/pps_tod_watchdog.conf
systemctl restart pps_tod_watchdog
sleep 3
systemctl is-active pps_tod_watchdog pps_tod chrony
echo '--- watchdog tail:'
tail -n 3 /var/log/pps_tod/watchdog.log
echo '--- state:'
cat /run/pps_tod/watchdog.state
echo WD-PLAN-A-DEPLOYED

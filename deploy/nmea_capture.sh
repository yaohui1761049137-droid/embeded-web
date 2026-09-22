#!/bin/bash
# 5-second raw NMEA capture from ttyS7 (pps_tod owns the port: brief pause).
set -e
systemctl stop pps_tod pps_tod_watchdog
sleep 1
stty -F /dev/ttyS7 115200 raw -echo 2>/dev/null || stty -F /dev/ttyS7 115200 raw -echo
timeout 5 cat /dev/ttyS7 > /tmp/nmea_capture.txt 2>/dev/null || true
systemctl start pps_tod
systemctl start pps_tod_watchdog
sleep 3
echo '== sentence type census =='
sed -n 's/^\(.\{6\}\).*/\1/p' /tmp/nmea_capture.txt | sort | uniq -c | sort -rn
echo '== first 20 lines =='
head -20 /tmp/nmea_capture.txt
echo CAPTURE-DONE

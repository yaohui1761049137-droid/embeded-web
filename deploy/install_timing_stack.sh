#!/bin/bash
# Deploy the PPS+TOD timing stack (route 1) on LubanCat 2N, per
# C:\Users\YAO\Desktop\codex_store\PPS_TOD\鲁班猫2N-PPS-TOD-chrony-实施记录.md
# This board: TOD serial = /dev/ttyS7 (uart7-m1 overlay), PPS = /dev/gpiochip3 line5.
set -e

echo '== compile pps_tod =='
gcc -O2 -Wall -o /usr/local/bin/pps_tod /tmp/pps_tod.c -lpthread 2>/tmp/gcc_pps_tod.log || { tail -5 /tmp/gcc_pps_tod.log; exit 1; }
echo '== scripts + conf =='
cp /tmp/pps_tod_watchdog.sh /tmp/pps_tod_rtc_save.sh /tmp/sanitize-drift.sh /usr/local/bin/
chmod 755 /usr/local/bin/pps_tod_watchdog.sh /usr/local/bin/pps_tod_rtc_save.sh /usr/local/bin/sanitize-drift.sh
mkdir -p /etc/pps_tod /run/pps_tod /var/log/pps_tod
cp /tmp/pps_tod_watchdog.conf /etc/pps_tod/pps_tod_watchdog.conf
echo '== chrony drop-in =='
mkdir -p /etc/systemd/system/chrony.service.d
cp /tmp/chrony-restart.conf /etc/systemd/system/chrony.service.d/restart.conf
echo '== units =='
cp /tmp/pps_tod_watchdog.service /tmp/pps_tod_rtc.service /tmp/pps_tod_rtc.timer /tmp/sanitize-drift.service /etc/systemd/system/
cat > /etc/systemd/system/pps_tod.service <<'EOF'
[Unit]
Description=PPS+TOD receiver daemon (feeds chrony via SysV SHM 0)
After=network.target chrony.service

[Service]
Type=simple
ExecStart=/usr/local/bin/pps_tod -D -b 115200 -t /dev/ttyS7
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF
echo '== chrony refclock + makestep =='
grep -q '^refclock SHM 0' /etc/chrony/chrony.conf || {
  sed -i 's/^makestep .*/makestep 0.01 3/' /etc/chrony/chrony.conf
  printf '\n# PPS+TOD refclock from pps_tod (route 1: gpiochip3 line5 edge events)\nrefclock SHM 0 refid PPS precision 1e-6 poll 2 delay 0.05\n' >> /etc/chrony/chrony.conf
}
grep -E '^refclock|^makestep' /etc/chrony/chrony.conf
systemctl daemon-reload
systemctl enable --now sanitize-drift.service pps_tod.service pps_tod_watchdog.service pps_tod_rtc.timer
systemctl restart chrony
sleep 3
echo '== state =='
echo "active:  $(systemctl is-active pps_tod) $(systemctl is-active pps_tod_watchdog) $(systemctl is-active sanitize-drift) $(systemctl is-active chrony)"
echo "enabled: $(systemctl is-enabled pps_tod) $(systemctl is-enabled pps_tod_watchdog) $(systemctl is-enabled pps_tod_rtc.timer) $(systemctl is-enabled sanitize-drift)"
ls -l /run/pps_tod/ 2>&1
echo TIMING-STACK-INSTALLED

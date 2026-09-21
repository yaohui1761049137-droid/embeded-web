#!/bin/bash
# System integration for embeded-web on LubanCat (README steps 3 / 3b / 3c / 4).
set -e

# ---- Step 3: www-data sudoers whitelist (nmcli + rollback watchdog arming)
cat > /etc/sudoers.d/99-www-nmcli <<'EOF'
www-data ALL=(ALL) NOPASSWD: /usr/bin/nmcli -t -f NAME\,UUID connection show, /usr/bin/nmcli -t connection show *, /usr/bin/nmcli -t device show *, /usr/bin/nmcli connection show *, /usr/bin/nmcli connection add type ethernet ifname *, /usr/bin/nmcli connection modify *, /usr/bin/nmcli connection up *, /usr/bin/systemd-run --on-active=180 /usr/local/bin/rollback_watchdog.sh
EOF
chmod 440 /etc/sudoers.d/99-www-nmcli
visudo -cf /etc/sudoers.d/99-www-nmcli

# ---- Step 3b: dialout group (serial devices for www-data)
usermod -aG dialout www-data
systemctl restart lighttpd

# ---- Step 3c: NTP monitoring stack
cp /tmp/chrony_acl_apply.sh /tmp/ntp_stats_sample.sh /usr/local/bin/
chmod 755 /usr/local/bin/chrony_acl_apply.sh /usr/local/bin/ntp_stats_sample.sh
cp /tmp/ntp-stats.service /tmp/ntp-stats.timer /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now ntp-stats.timer

gcc -O2 -Wall -o /usr/local/bin/ntp_nic_monitor /tmp/ntp_nic_monitor.c
cp /tmp/ntp-nic-monitor.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now ntp-nic-monitor

printf "# Web-managed NTP access rules (chrony allow/deny) - do not edit by hand.\nallow 192.168.137.0/24\n" > /etc/chrony/acl-web.conf
chmod 644 /etc/chrony/acl-web.conf
grep -q "include /etc/chrony/acl-web.conf" /etc/chrony/chrony.conf || {
  sed -i '/^allow /d; /^deny /d' /etc/chrony/chrony.conf
  echo "include /etc/chrony/acl-web.conf" >> /etc/chrony/chrony.conf
}
systemctl restart chrony

cat > /etc/sudoers.d/99-www-ntpacl <<'EOF'
www-data ALL=(root) NOPASSWD: /usr/local/bin/chrony_acl_apply.sh add allow *, /usr/local/bin/chrony_acl_apply.sh add deny *, /usr/local/bin/chrony_acl_apply.sh remove *
EOF
chmod 440 /etc/sudoers.d/99-www-ntpacl
visudo -cf /etc/sudoers.d/99-www-ntpacl

# ---- Step 4: rollback watchdog + boot recovery
cp /tmp/rollback_watchdog.sh /usr/local/bin/
chmod 755 /usr/local/bin/rollback_watchdog.sh
cp /tmp/rollback-recover.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable rollback-recover.service

echo INTEGRATION-OK
echo "active:   $(systemctl is-active lighttpd) $(systemctl is-active chrony) $(systemctl is-active ntp-nic-monitor) $(systemctl is-active ntp-stats.timer)"
echo "enabled:  $(systemctl is-enabled lighttpd) $(systemctl is-enabled chrony) $(systemctl is-enabled ntp-nic-monitor) $(systemctl is-enabled ntp-stats.timer) $(systemctl is-enabled rollback-recover)"

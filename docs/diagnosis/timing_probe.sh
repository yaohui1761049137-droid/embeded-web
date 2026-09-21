#!/bin/bash
# Inspect ttyS7 (TOD serial) and GPIO3_A5 (PPS input) configuration on LubanCat.
echo '=== ttyS7 device ==='
ls -l /dev/ttyS* 2>/dev/null

echo '=== dmesg: uart/serial nodes ==='
dmesg | grep -iE 'ttyS|uart' | head -n 20

echo '=== pps devices ==='
ls -l /dev/pps* 2>/dev/null || echo '(no /dev/pps*)'
dmesg | grep -iE 'pps|gpio3' | head -n 12

echo '=== gpio class ==='
ls /sys/class/gpio/ 2>/dev/null
grep -iE 'gpio3|pps' /sys/kernel/debug/gpio 2>/dev/null | head -n 8

echo '=== device-tree nodes (pps/uart) ==='
for n in $(ls /proc/device-tree/ 2>/dev/null | grep -iE 'pps|uart'); do
  st=$(cat /proc/device-tree/$n/status 2>/dev/null | tr -d '\0')
  compat=$(cat /proc/device-tree/$n/compatible 2>/dev/null | tr -d '\0')
  echo "node $n  status=$st  compatible=$compat"
done

echo '=== vendor timing services ==='
systemctl list-unit-files 2>/dev/null | grep -iE 'pps|tod|gnss|timing'
ls /etc/init.d/ 2>/dev/null | grep -iE 'pps|tod|gnss'
pgrep -af 'pps|tod|gnss' | grep -v pgrep

echo '=== pps_tod log source dir ==='
ls -l /var/log/ 2>/dev/null | grep -iE 'pps|tod|gnss'

#!/bin/bash
# IO-domain / RTC / GPIO3 evidence probe (healthy-state baseline).
echo '=== io-domains DT node ==='
ls /proc/device-tree/ | grep -i domain
ls /proc/device-tree/io-domains/ 2>/dev/null | head -12
echo
echo '=== io-domains supply strings ==='
for f in /proc/device-tree/io-domains/*; do
  case "$(basename "$f")" in
    *-supply) echo "$f -> $(od -c -N4 "$f" | head -1)";;
    *) [ -s "$f" ] && { printf '%s: ' "$(basename "$f")"; tr -d '\0' < "$f"; echo; };;
  esac
done 2>/dev/null | head -20
echo
echo '=== which regulator phandle feeds vccio ==='
for ph in /sys/kernel/debug/regulator/*/name; do :; done 2>/dev/null
grep -iE 'name|vccio' /sys/kernel/debug/regulator/regulator_summary 2>/dev/null | head -20
echo
echo '=== dmesg: domain / i2c0 / hym8563 / rk808 ==='
dmesg | grep -iE 'domain' | head -6
dmesg | grep -iE 'hym8563|fdd40000|i2c.*0-00(20|51)' | head -10
echo
echo '=== hym8563 driver presence ==='
ls -l /sys/class/rtc/rtc0/device/driver 2>/dev/null
echo PROBE-DONE

#!/bin/bash
# boot_probe.sh — evidence recorder for the intermittent low-speed-IO power-up
# failure (GPIO3/RTC dead at some power-ups, recovered by full power cycle).
# Runs at every boot (boot-probe.service); phase 1 immediately, phase 2 after
# 120s (by then pps_tod should have produced samples if the IO domain is up).
set -u
OUT=/var/db/boot_probe
LOG=$OUT/probe.log
mkdir -p "$OUT"
N=$(ls "$OUT"/dmesg_boot*.txt 2>/dev/null | wc -l); N=$((N+1))
DM=$OUT/dmesg_boot$(printf '%02d' $N).txt

{
echo "=== BOOT #$N  $(date '+%F %T')  btime=$(grep btime /proc/stat | awk '{print $2}') ==="
echo "-- hwclock -r:"
hwclock -r 2>&1
echo "-- date:"
date '+%F %T'
echo "-- hym8563 / setting system clock:"
dmesg | grep -iE 'hym8563|setting system clock' | head -4
echo "-- regulator/io-domain errors at boot:"
dmesg | grep -iE 'regulator.*(fail|error|defer|not ready|disabled)|LDO_REG[45]|vccio' | head -12
echo "-- i2c0 errors at boot:"
dmesg | grep -iE 'fdd40000|i2c.*(fail|error|timeout|nack)' | head -8
echo "-- gpio3/uart7 probe:"
dmesg | grep -iE 'fe760000|gpio3|fe6a0000|serial' | head -6
echo "-- pinmux 96-127 (first 8):"
grep -E 'pin (9[6-9]|1[01][0-9]|12[0-7]) ' /sys/kernel/debug/pinctrl/pinctrl-rockchip-pinctrl/pinmux-pins 2>/dev/null | head -8
echo "-- vccio rails now:"
grep -E 'vccio_sd|vccio_acodec|vcc_1v8|vcc3v3_pmu' /sys/kernel/debug/regulator/regulator_summary 2>/dev/null
} >> "$LOG" 2>&1
cp /var/log/dmesg "$DM" 2>/dev/null || dmesg > "$DM" 2>/dev/null

# phase 2: after pps_tod has had 120s, record whether samples flow
(
  sleep 120
  {
  echo "=== BOOT#${N} +120s ==="
  echo "-- status:"; cat /run/pps_tod/status 2>/dev/null | head -6
  echo "-- sources:"; chronyc sources 2>/dev/null | tail -2
  echo "-- hwclock now:"; hwclock -r 2>&1
  echo
  } >> "$LOG" 2>&1
) &
exit 0

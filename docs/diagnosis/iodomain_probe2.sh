#!/bin/bash
# Map the VCCIO3 <- vccio_sd <- rk808-LDO supply chain from DT + regulator tree.
echo '=== DT: vccio_sd / io-domains nodes ==='
find /proc/device-tree -maxdepth 2 -name '*io-domain*' 2>/dev/null
find /proc/device-tree -maxdepth 3 -name 'vccio*' 2>/dev/null | head -5
echo
echo '=== DT: vccio_sd-supply phandle value ==='
for d in /proc/device-tree/*/io-domains /proc/device-tree/io-domains; do
  [ -d "$d" ] && for f in "$d"/*-supply; do
    [ -e "$f" ] || continue
    ph=$(od -A n -t x1 -N4 "$f" | tr -d ' \n')
    echo "$(basename "$f"): phandle=0x$ph"
  done
done
echo
echo '=== DT: which rk808 reg has that phandle ==='
for p in /proc/device-tree/*/regulators/* /proc/device-tree/*/*/regulators/*; do
  [ -e "$p/phandle" ] || continue
  hp=$(od -A n -t x1 -N4 "$p/phandle" | tr -d ' \n')
  [ -n "$hp" ] && echo "$p phandle=0x$hp"
done 2>/dev/null | head -16
echo
echo '=== regulator_summary (rk808 ldo names + voltages) ==='
cat /sys/kernel/debug/regulator/regulator_summary 2>/dev/null | grep -E 'vccio|DCDC|LDO|vcc' | head -22
echo
echo '=== healthy-boot RTC read (dmesg evidence) ==='
dmesg | grep -iE 'hym8563|setting system clock'
echo PROBE2-DONE

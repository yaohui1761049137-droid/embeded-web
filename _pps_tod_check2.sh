echo '== DT serial nodes (any level) =='
find /proc/device-tree -maxdepth 2 -name '*serial@*' 2>/dev/null
for d in $(find /proc/device-tree -maxdepth 2 -name '*serial@*' 2>/dev/null); do
  echo "$d status=$(cat $d/status 2>/dev/null | tr -d '\0') compatible=$(cat $d/compatible 2>/dev/null | tr -d '\0' | head -c 40)"
done
echo '== kernel pps support =='
find /lib/modules/$(uname -r) -name 'pps*' 2>/dev/null | head -n 6
grep -i pps /proc/modules | head -n 4
echo '== boot cmdline console =='
cat /proc/cmdline
echo '== gpio3 chip base confirm =='
cat /sys/class/gpio/gpiochip96/label 2>/dev/null
cat /sys/class/gpio/gpiochip96/base 2>/dev/null

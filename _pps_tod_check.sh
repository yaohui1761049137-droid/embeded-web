echo '== 1. ttyS7 device node =='
ls -l /dev/ttyS7 2>/dev/null || echo 'no /dev/ttyS7'
echo '== 2. uart/serial dmesg =='
dmesg | grep -iE 'ttyS7|uart' | head -n 12
echo '== 3. DT uart nodes status =='
for d in /proc/device-tree/*uart*; do
  [ -d "$d" ] && echo "$d status=$(cat $d/status 2>/dev/null | tr -d '\0')"
done
echo '== 4. ttyS7 driver binding =='
ls -l /sys/class/tty/ttyS7/device/driver 2>/dev/null || echo 'no driver symlink'
echo '== 5. PPS device nodes =='
ls -l /dev/pps* 2>/dev/null || echo 'no /dev/pps*'
dmesg | grep -iE '\bpps\b' | head -n 8
echo '== 6. DT pps/gnss nodes =='
ls /proc/device-tree/ | grep -iE 'pps|gnss'
for d in /proc/device-tree/*pps*; do
  [ -d "$d" ] && { echo "node $d:"; for f in $d/*; do
    case "$f" in *compatible*|*status*|*gpios*|*assert*|*name*)
      echo "  $(basename $f) = $(cat $f 2>/dev/null | tr -d '\0' | head -c 60)";;
    esac
  done; }
done
echo '== 7. debugfs gpio (GPIO3_A5 = gpiochip3 pin 5 -> num 101) =='
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug 2>/dev/null
grep -iE 'gpio3|pps' /sys/kernel/debug/gpio 2>/dev/null | head -n 6
ls /sys/class/gpio/ 2>/dev/null | head -n 10
echo '== 8. pinctrl pin101 state =='
grep -E 'pin 101 |:.*gpio3' /sys/kernel/debug/pinctrl/pinctrl-rockchip-pinctrl/pinmux-pins 2>/dev/null | head -n 3
echo '== 9. timing-related services =='
systemctl list-unit-files 2>/dev/null | grep -iE 'pps|tod|gnss'
ls /etc/init.d/ | grep -iE 'pps|tod|gnss'
echo '== 10. timing log source =='
ls -l /var/log/pps* /var/log/*tod* 2>/dev/null || echo 'no pps/tod logs'

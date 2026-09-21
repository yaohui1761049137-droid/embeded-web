#!/bin/bash
# Enable uart7 (M1, 40pin 35/37) via the vendor overlay mechanism per
# https://doc.embedfire.com/linux/rk356x/quick_start/.../40pin/uart/uart.html
set -e
cd /boot/uEnv
cp uEnv.txt uEnv.txt.bak-before-uart7
sed -i 's|^#dtoverlay=/dtb/overlay/rk356x-lubancat-uart7-m1-overlay.dtbo|dtoverlay=/dtb/overlay/rk356x-lubancat-uart7-m1-overlay.dtbo|' uEnv.txt
sync
echo '=== active lines ==='
grep -n 'uart7' uEnv.txt
echo '=== backup ==='
ls -l uEnv.txt.bak-before-uart7
echo UART7-OVERLAY-ENABLED

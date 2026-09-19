#!/bin/bash
# pinpoint.sh — 12kpps/口 30s，每 2s 五点采样定位损失跳数
set -u
WINDIR_WIN='C:\Users\YAO\AppData\Local\Temp\ntpramp'
WINDIR=/mnt/c/Users/YAO/AppData/Local/Temp/ntpramp
WIN_PY='C:\Users\YAO\AppData\Local\Programs\Python\Python310\python.exe'
bsh() {
  local b64=$(base64 -w0 | tr -d '\n')
  printf '%s' "$b64" > "$WINDIR/payload.b64"
  timeout 60 powershell.exe -NoProfile -ExecutionPolicy Bypass \
    -File "$WINDIR_WIN\bssh.ps1" -B64File "$WINDIR_WIN\payload.b64" -Board 192.168.137.100 2>&1 | tr -d '\r'
}
# 五点采样: e0in e0out e1in e1out hits Rcvbuf sockdrop
snap5() { bsh <<'EOF'
for i in 0 1; do
  n=$(iptables -nvx -L INPUT 2>/dev/null | awk -v ifn="eth$i" '$3=="ACCEPT" && $4=="udp" && $6==ifn && /udp dpt:123/ {print $1; exit}')
  m=$(iptables -nvx -L OUTPUT 2>/dev/null | awk -v ifn="eth$i" '$3=="ACCEPT" && $4=="udp" && $7==ifn && /udp spt:123/ {print $1; exit}')
  printf '%s %s ' "$n" "$m"
done
SS=$(chronyc -c serverstats 2>/dev/null | tr -d '\r'); echo "HITS ${SS%%,*}"
awk '/^Udp:/ {for(i=1;i<NF;i++) if($i=="RcvbufErrors") c=i+1; if(c) print "RB " $c}' /proc/net/snmp | tail -1
awk '/:00BB/ {print "SOCKQ " $2" "$3" "$NF}' /proc/net/udp | head -3
EOF
}
echo "t e0in e0out e1in e1out hits RcvbufErr sockq(tx rx drops)"
A=$(snap5); tA=$(date +%s)
rm -f "$WINDIR/e0.pid" "$WINDIR/e1.pid"
for t in e0 e1; do :; done
flood() {
  local tag="$5"
  local args="\"$WINDIR_WIN\\ntpflood.py\",'$1','$2','$3','$4'"
  powershell.exe -NoProfile -Command \
    "\$p = Start-Process -FilePath '$WIN_PY' -ArgumentList $args -WindowStyle Hidden -PassThru -RedirectStandardOutput '$WINDIR_WIN\\${tag}_out.log' -RedirectStandardError '$WINDIR_WIN\\${tag}_err.log'; \$p.Id | Out-File -FilePath '$WINDIR_WIN\\${tag}.pid' -Encoding ascii" \
    >/dev/null 2>&1 &
}
flood 192.168.137.11 192.168.137.100 12000 35 "e0" &
flood 192.168.1.121 192.168.1.150 12000 35 "e1" &
echo "BASE $A"
for k in 1 2 3 4 5 6 7 8 9 10 11 12 13; do
  sleep 2
  echo "T$k $(snap5)"
done
for t in e0 e1; do
  pid=$(cat "$WINDIR/$t.pid" 2>/dev/null)
  [ -n "$pid" ] && timeout 20 powershell.exe -NoProfile -Command "Stop-Process -Id $pid -Force -ErrorAction SilentlyContinue" >/dev/null 2>&1
done
sleep 1
echo "END $(snap5)"

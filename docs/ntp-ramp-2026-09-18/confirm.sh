#!/bin/bash
# confirm.sh — 暂停 sampler 后复测 8k/12k 两档应答率（每档 40s，无整分干扰）
set -u
WINDIR_WIN='C:\Users\YAO\AppData\Local\Temp\ntpramp'
WINDIR=/mnt/c/Users/YAO/AppData/Local/Temp/ntpramp
WIN_PY='C:\Users\YAO\AppData\Local\Programs\Python\Python310\python.exe'
BOARD=192.168.137.100; ETH1=192.168.1.150
SRC0=192.168.137.11; SRC1=192.168.1.121
bsh() {
  local b64=$(base64 -w0 | tr -d '\n')
  printf '%s' "$b64" > "$WINDIR/payload.b64"
  timeout 60 powershell.exe -NoProfile -ExecutionPolicy Bypass \
    -File "$WINDIR_WIN\bssh.ps1" -B64File "$WINDIR_WIN\payload.b64" -Board 192.168.137.100 2>&1 | tr -d '\r'
}
snap() { bsh <<'EOF'
for i in 0 1; do
  n=$(iptables -nvx -L INPUT 2>/dev/null | awk -v ifn="eth$i" '$3=="ACCEPT" && $4=="udp" && $6==ifn && /udp dpt:123/ {print $1; exit}')
  m=$(iptables -nvx -L OUTPUT 2>/dev/null | awk -v ifn="eth$i" '$3=="ACCEPT" && $4=="udp" && $7==ifn && /udp spt:123/ {print $1; exit}')
  printf 'e%s %s %s\n' "$i" "${n:-0}" "${m:-0}"
done
awk '/^Udp:/ {for(i=1;i<NF;i++) if($i=="RcvbufErrors") c=i+1; if(c) print "RB " $c}' /proc/net/snmp | tail -1
EOF
}
flood() {
  local tag="$5"
  local args="\"$WINDIR_WIN\\ntpflood.py\",'$1','$2','$3','$4'"
  powershell.exe -NoProfile -Command \
    "\$p = Start-Process -FilePath '$WIN_PY' -ArgumentList $args -WindowStyle Hidden -PassThru -RedirectStandardOutput '$WINDIR_WIN\\${tag}_out.log' -RedirectStandardError '$WINDIR_WIN\\${tag}_err.log'; \$p.Id | Out-File -FilePath '$WINDIR_WIN\\${tag}.pid' -Encoding ascii" \
    >/dev/null 2>&1 &
}
for rate in 8000 12000; do
  echo "== ${rate}pps/口 x40s =="
  A=$(snap); tA=$(date +%s)
  rm -f "$WINDIR/e0.pid" "$WINDIR/e1.pid"
  flood "$SRC0" "$BOARD" "$rate" 45 "e0" & flood "$SRC1" "$ETH1" "$rate" 45 "e1" &
  sleep 40
  for t in e0 e1; do
    pid=$(cat "$WINDIR/$t.pid" 2>/dev/null)
    [ -n "$pid" ] && timeout 20 powershell.exe -NoProfile -Command "Stop-Process -Id $pid -Force -ErrorAction SilentlyContinue" >/dev/null 2>&1
  done
  sleep 2
  B=$(snap)
  echo "$A" | awk 'NR==FNR{a[$1"_"NR%100000]=$2" "$3; next} {print}' /dev/null >/dev/null
  python3 - "$A" "$B" <<'PY'
import sys
def parse(t):
    d = {}
    for line in t.splitlines():
        f = line.split()
        if f[0] in ('e0','e1'): d[f[0]] = (int(f[1]), int(f[2]))
        if f[0] == 'RB': d['RB'] = int(f[1])
    return d
a, b = parse(sys.argv[1]), parse(sys.argv[2])
for nic in ('e0','e1'):
    di = b[nic][0]-a[nic][0]; do = b[nic][1]-a[nic][1]
    print(f"  {nic}: req={di} rsp={do} rate={do/di*100:.1f}%")
print(f"  RcvbufErr delta: {b['RB']-a['RB']}")
PY
done

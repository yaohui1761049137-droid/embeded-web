#!/bin/bash
# ramp_test3.sh — NTP 请求量爬升压测 v3
#
# 需求：对 eth0/eth1 持续 10 分钟 NTP 压测；单位时间请求量由低到高爬升；
# 当应答率下降到 80% 时，单位时间请求量再由高到低回落。
#
# 流程：
#   阶段 A 爬升：500→1k→2k→4k→8k→16k→20k→24k pps/口（双口对称），每档 ≤40s
#     任一口连续两个有效采样窗应答率 <80% → 立即转回落
#     阶梯走完仍未触发 → 从最高档转回落（兜底）
#   阶段 B 回落：16k→12k→8k→6k→4k→3k→2k→1.5k→1k→500，每档 ≤25s
#     连续两窗恢复 ≥95% 时记录恢复点
#
# 相对 v2 的修复：
#   1) 进程管理：Start-Process -PassThru 记录 python PID → Stop-Process 按 PID
#      杀（v2 kill powershell 时 python 被孤儿化，18 个残留发包器叠加发包，
#      数据全污染）；发包端另有 secs=budget+20s 自然超时兜底。
#   2) 档位起始自检：第一窗若 win_req=0 视为发包失败，打印发包端 stderr。
#   3) 快照含 /proc/net/snmp Udp RcvbufErrors（chronyd 丢包的内核侧佐证）。
#   4) CSV 记录真实窗长 win_secs（sleep 5 + 采样耗时 ≈ 6~13s，压测负载下变长）。
set -u
WINDIR_WIN='C:\Users\YAO\AppData\Local\Temp\ntpramp'
WINDIR=/mnt/c/Users/YAO/AppData/Local/Temp/ntpramp
WIN_PY='C:\Users\YAO\AppData\Local\Programs\Python\Python310\python.exe'
BOARD=192.168.137.100
ETH1=192.168.1.150
SRC0=192.168.137.11
SRC1=192.168.1.121
DATA=/tmp/ntpramp/ramp_data.csv
LOG=/tmp/ntpramp/ramp_log.txt
UP_STEPS="500 1000 2000 4000 8000 16000 20000 24000"
DOWN_STEPS="16000 12000 8000 6000 4000 3000 2000 1000 500"
UP_SECS=40
DOWN_SECS=25
SAMPLE_SECS=5
RATE_FLOOR=80
RECOVER=95

: > "$DATA"; : > "$LOG"
echo "phase,step_pps,t_rel,epoch,win_secs,skip,e0_req,e0_rsp,e0_rate,e1_req,e1_rsp,e1_rate,rcvbuf" >> "$DATA"

bsh() {
  local b64
  b64=$(base64 -w0 | tr -d '\n')
  printf '%s' "$b64" > "$WINDIR/payload.b64"
  timeout 60 powershell.exe -NoProfile -ExecutionPolicy Bypass \
    -File "$WINDIR_WIN\bssh.ps1" \
    -B64File "$WINDIR_WIN\payload.b64" -Board "$BOARD" 2>&1 | tr -d '\r'
}

# 板端快照: "e0 <in> <out>" ×2 + "RB <RcvbufErrors>"
# 注意: /proc/net/snmp 里 Udp: 值行后面紧跟 UdpLite: 表头，-A1 会把它带进来，
# 必须取 -A1 输出的第 2 行（Udp 值行）而不是 tail -1。
snap() {
  bsh <<'EOF'
for i in 0 1; do
  n=$(iptables -nvx -L INPUT 2>/dev/null | awk -v ifn="eth$i" '$3=="ACCEPT" && $4=="udp" && $6==ifn && /udp dpt:123/ {print $1; exit}')
  m=$(iptables -nvx -L OUTPUT 2>/dev/null | awk -v ifn="eth$i" '$3=="ACCEPT" && $4=="udp" && $7==ifn && /udp spt:123/ {print $1; exit}')
  printf 'e%s %s %s\n' "$i" "${n:-0}" "${m:-0}"
done
awk '/^Udp:/ {for(i=1;i<NF;i++) if($i=="RcvbufErrors") c=i+1; if(c) print "RB " $c}' /proc/net/snmp | tail -1
EOF
}

flood() { # flood <src> <dst> <rate> <secs> <tag> — PID 写 <tag>.pid，输出重定向
  # 注意: ${tag} 必须在 bash 侧展开（\$ 只用于 PowerShell 变量 $p）
  local tag="$5"
  local args="\"$WINDIR_WIN\\ntpflood.py\",'$1','$2','$3','$4'"
  powershell.exe -NoProfile -Command \
    "\$p = Start-Process -FilePath '$WIN_PY' -ArgumentList $args -WindowStyle Hidden -PassThru -RedirectStandardOutput '$WINDIR_WIN\\${tag}_out.log' -RedirectStandardError '$WINDIR_WIN\\${tag}_err.log'; \$p.Id | Out-File -FilePath '$WINDIR_WIN\\${tag}.pid' -Encoding ascii" \
    >/dev/null 2>&1 &
}

log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

RATE_BREAK=0
RECOVER_AT=""
badrun=0; goodrun=0
PREV_T=0

run_phase() { # run_phase <phase> <steps...>
  local ph="$1"; shift
  local rate
  for rate in "$@"; do
    local budget=$UP_SECS
    [ "$ph" = B ] && budget=$DOWN_SECS
    log "== 阶段$ph 档位 ${rate}pps/口 开始 =="
    : > "$WINDIR/e0_out.log"; : > "$WINDIR/e0_err.log"
    : > "$WINDIR/e1_out.log"; : > "$WINDIR/e1_err.log"

    local A0; A0=$(snap)
    local p_in0 p_out0 p_in1 p_out1 p_rb
    p_in0=$(echo "$A0" | awk '$1=="e0"{print $2}'); p_out0=$(echo "$A0" | awk '$1=="e0"{print $3}')
    p_in1=$(echo "$A0" | awk '$1=="e1"{print $2}'); p_out1=$(echo "$A0" | awk '$1=="e1"{print $3}')
    p_rb=$(echo "$A0" | awk '$1=="RB"{print $2}'); [ -n "$p_rb" ] || p_rb=0
    PREV_T=$(date +%s)

    # 发包端：自然超时 budget+20s 兜底，正常由 Stop-Process 提前终止
    rm -f "$WINDIR/e0.pid" "$WINDIR/e1.pid"
    flood "$SRC0" "$BOARD" "$rate" "$((budget+20))" "e0" &
    flood "$SRC1" "$ETH1"  "$rate" "$((budget+20))" "e1" &
    local pid0="" pid1="" try
    for try in 1 2 3 4 5; do
      sleep 1
      [ -z "$pid0" ] && pid0=$(cat "$WINDIR/e0.pid" 2>/dev/null)
      [ -z "$pid1" ] && pid1=$(cat "$WINDIR/e1.pid" 2>/dev/null)
      [ -n "$pid0" ] && [ -n "$pid1" ] && break
    done
    [ -n "$pid0" ] && [ -n "$pid1" ] || log "   警告: PID 文件缺失 e0=$pid0 e1=$pid1"

    local step_start=$(date +%s) firstwin=1
    while :; do
      sleep $SAMPLE_SECS
      local now=$(date +%s)
      local t_rel=$(( now - T0 )) el=$(( now - step_start ))
      [ "$el" -ge "$budget" ] && break
      local B0; B0=$(snap)
      local bin0 bout0 bin1 bout1 rb
      bin0=$(echo "$B0" | awk '$1=="e0"{print $2}'); bout0=$(echo "$B0" | awk '$1=="e0"{print $3}')
      bin1=$(echo "$B0" | awk '$1=="e1"{print $2}'); bout1=$(echo "$B0" | awk '$1=="e1"{print $3}')
      rb=$(echo "$B0" | awk '$1=="RB"{print $2}'); [ -n "$rb" ] || rb=0
      local win=$(( now - PREV_T ))

      local skip=0 reason=""
      # 整分横跨作废（ntp-stats.timer 每分钟重置 OUTPUT 计数域）
      local s sec_now=$(( 10#$(date +%S) ))
      for s in $(seq $(( sec_now - win )) $sec_now); do
        if [ $(( (s + 120) % 60 )) -le 1 ]; then skip=1; reason="minute-boundary"; fi
      done
      # 计数器回退 → 重建基线作废本窗
      if [ "$bin0" -lt "$p_in0" ] || [ "$bout0" -lt "$p_out0" ] || \
         [ "$bin1" -lt "$p_in1" ] || [ "$bout1" -lt "$p_out1" ]; then
        skip=1; reason="${reason:+$reason }counter-reset"
      fi
      if [ "$skip" = 1 ]; then
        echo "$ph,$rate,$t_rel,$(date +%s),$win,SKIP:$reason,$bin0,$bout0,-1,$bin1,$bout1,-1,$rb" >> "$DATA"
        log "  ${rate}pps t+${t_rel}s [作废: $reason]"
        p_in0=$bin0; p_out0=$bout0; p_in1=$bin1; p_out1=$bout1; PREV_T=$(date +%s)
        continue
      fi

      local calc d0i d0o r0 d1i d1o r1
      calc=$(awk -v pi0="$p_in0" -v po0="$p_out0" -v pi1="$p_in1" -v po1="$p_out1" \
        -v ci0="$bin0" -v co0="$bout0" -v ci1="$bin1" -v co1="$bout1" 'BEGIN{
        d0i=ci0-pi0; d0o=co0-po0; d1i=ci1-pi1; d1o=co1-po1;
        r0=(d0i>0)?d0o/d0i*100:-1; r1=(d1i>0)?d1o/d1i*100:-1;
        printf "%d %d %.1f %d %d %.1f", d0i,d0o,r0,d1i,d1o,r1 }')
      d0i=${calc%% *}; rest=${calc#* }; d0o=${rest%% *}; rest=${rest#* }; r0=${rest%% *}
      rest=${rest#* }; d1i=${rest%% *}; rest=${rest#* }; d1o=${rest%% *}; r1=${rest##* }
      echo "$ph,$rate,$t_rel,$(date +%s),$win,ok,$d0i,$d0o,$r0,$d1i,$d1o,$r1,$rb" >> "$DATA"
      local pps0=$(( d0i * 10 / win )) pps1=$(( d1i * 10 / win ))
      log "  ${rate}pps t+${t_rel}s w=${win}s e0: req=$d0i(${pps0}pps) rate=${r0}% | e1: req=$d1i(${pps1}pps) rate=${r1}% | RcvbufErrΔ=$(( rb - p_rb ))"

      # 档位起始自检：第一有效窗流量为 0 → 发包失败
      if [ "$firstwin" = 1 ]; then
        firstwin=0
        if [ "$d0i" -eq 0 ] || [ "$d1i" -eq 0 ]; then
          log "   !! 发包异常 e0req=$d0i e1req=$d1i"
          log "      e0err: $(tail -c 300 "$WINDIR/e0_err.log" 2>/dev/null | tr '\n' ' ')"
          log "      e1err: $(tail -c 300 "$WINDIR/e1_err.log" 2>/dev/null | tr '\n' ' ')"
        fi
      fi

      p_in0=$bin0; p_out0=$bout0; p_in1=$bin1; p_out1=$bout1; p_rb=$rb; PREV_T=$(date +%s)

      if [ "$ph" = A ]; then
        local hit=0
        awk -v a="$r0" -v f="$RATE_FLOOR" 'BEGIN{exit !(a<f)}' && [ "$d0i" -ge 1000 ] && hit=1
        awk -v b="$r1" -v f="$RATE_FLOOR" 'BEGIN{exit !(b<f)}' && [ "$d1i" -ge 1000 ] && hit=1
        [ "$hit" = 1 ] && badrun=$((badrun+1)) || badrun=0
        if [ "$badrun" -ge 2 ]; then
          RATE_BREAK=1
          log "!! 应答率连续两窗 < ${RATE_FLOOR}%（e0=${r0}% e1=${r1}%），触发回落"
          break
        fi
      else
        local rec=1
        awk -v a="$r0" -v v="$RECOVER" 'BEGIN{exit !(a<v)}' && rec=0
        awk -v b="$r1" -v v="$RECOVER" 'BEGIN{exit !(b<v)}' && rec=0
        if [ "$rec" = 1 ] && [ "$d0i" -ge 500 ]; then goodrun=$((goodrun+1)); else goodrun=0; fi
        if [ "$goodrun" -ge 2 ] && [ -z "$RECOVER_AT" ]; then
          RECOVER_AT="${rate}pps t+${t_rel}s"
          log "   ✓ 应答率恢复 ≥${RECOVER}%（恢复点：${RECOVER_AT}）"
        fi
      fi
    done

    # 按 PID 直接杀 python（孤儿防护）
    for tag in e0 e1; do
      local pid; pid=$(cat "$WINDIR/$tag.pid" 2>/dev/null)
      [ -n "$pid" ] && timeout 20 powershell.exe -NoProfile -Command \
        "Stop-Process -Id $pid -Force -ErrorAction SilentlyContinue" >/dev/null 2>&1
    done
    sleep 2
    log "   档位 ${rate} 结束 (e0: $(tail -c 120 "$WINDIR/e0_out.log" 2>/dev/null|tr '\n' ' ')$(tail -c 200 "$WINDIR/e0_err.log" 2>/dev/null|tr '\n' ' ') | e1: $(tail -c 120 "$WINDIR/e1_out.log" 2>/dev/null|tr '\n' ' ')$(tail -c 200 "$WINDIR/e1_err.log" 2>/dev/null|tr '\n' ' '))"
    [ "$ph" = A ] && [ "$RATE_BREAK" = 1 ] && return 0
    badrun=0
  done
}

T0=$(date +%s)
log "== 压测开始：双口对称爬升 $UP_STEPS =="
run_phase A $UP_STEPS
log "== 切换到阶段 B（高→低回落）: $DOWN_STEPS =="
run_phase B $DOWN_STEPS
log "== 压测结束 =="
[ -n "$RECOVER_AT" ] && log "恢复点: $RECOVER_AT"
echo ""
column -t -s, "$DATA"

#!/bin/bash
# test_ntp_nic.sh — 逐网口 NTP 流量监控端到端测试
#
# 验证「服务监控及报警」Tab 的各网口柱状图链路：
#     iptables 每网口 udp/123 计数 → /var/db/ntp_stats.csv → ntpmon.cgi JSON
#
# 关键约束（决定测试怎么发流量）：
#   计数规则插在 INPUT 链上（-i ethN -p udp --dport 123），所以只有"从外部、
#   经该物理网口进来、目的地址是本机"的 UDP/123 包才会落到该网口。板子自己
#   发出去的包走 OUTPUT 链不计；发往本机另一个网口地址的包被内核走 lo 也不计。
#   因此每个网口都必须有真实对端设备往它发包。
#
#   PC 侧若有两块网卡处于同一网段，Windows 对同一目的地会有两条等价路由，
#   包可能从错误的网口出去。本脚本用"绑定源地址"强制出口网卡（源地址路由），
#   并在探针阶段先用物理收包计数（/sys/class/net/ethN/statistics/rx_packets）
#   确认归属，再开始正式断言 —— 归属不成立时立即停止。
#
# 用法：
#   ./test_ntp_nic.sh [选项]
#
#   --board-ip IP    板子 eth0 侧地址（默认 192.168.137.100）
#   --eth1-ip IP     板子 eth1 侧地址（默认自动探测）
#   --src-eth0 IP    PC 在 eth0 链路一侧的源地址（绑定用，默认 192.168.137.11）
#   --src-eth1 IP    PC 在 eth1 链路一侧的源地址（绑定用，默认 192.168.1.121）
#   --n1 N           eth0 侧注入包数（默认 50）
#   --n2 N           eth1 侧注入包数（默认 70）
#   --quick          跳过静默基线观察
#   --transport T    auto | local | winrelay（默认 auto）
#   --no-color
#
# 传输层：local 用本机 ssh/curl；winrelay 经 Windows 的 ssh.exe/curl.exe/Python
# 转发（WSL 镜像网络在双网卡同网段时无法直达该网段，需要这条路径）。
#
# 注意：脚本会往 /var/db/ntp_stats.csv 追加真实采样行，并可能临时修改 iptables。
# 不改变网口配置、不重启 chrony（除非配合 --with-acl 的规则删除）。

set -uo pipefail

BOARD_IP=${BOARD_IP:-192.168.137.100}
BOARD_SSH_PASS=${BOARD_SSH_PASS:-root}
ETH1_IP=${ETH1_IP:-}
SRC_ETH0=${SRC_ETH0:-192.168.137.11}
SRC_ETH1=${SRC_ETH1:-192.168.1.121}
N1=${N1:-50}
N2=${N2:-70}
WEB_USER=${WEB_USER:-root}
# Use the board's *documented* default (README: root/admin) rather than any
# real credential; override WEB_PASS when the password has been changed.
WEB_PASS=${WEB_PASS:-admin}
QUICK=0
TRANSPORT=${TRANSPORT:-auto}
USE_COLOR=1

while [ $# -gt 0 ]; do
    case "$1" in
        --board-ip)   BOARD_IP="$2"; shift 2 ;;
        --eth1-ip)    ETH1_IP="$2"; shift 2 ;;
        --src-eth0)   SRC_ETH0="$2"; shift 2 ;;
        --src-eth1)   SRC_ETH1="$2"; shift 2 ;;
        --n1)         N1="$2"; shift 2 ;;
        --n2)         N2="$2"; shift 2 ;;
        --quick)      QUICK=1; shift ;;
        --transport)  TRANSPORT="$2"; shift 2 ;;
        --no-color)   USE_COLOR=0; shift ;;
        -h|--help)    sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

PASS=0; FAIL=0
if [ "$USE_COLOR" = 1 ]; then G=$'\033[32m'; R=$'\033[31m'; Y=$'\033[33m'; Z=$'\033[0m';
else G=; R=; Y=; Z=; fi
ok()  { echo "  ${G}✅ PASS${Z} $1"; PASS=$((PASS+1)); }
bad() { echo "  ${R}❌ FAIL${Z} $1"; FAIL=$((FAIL+1)); }
info(){ echo "  ${Y}··${Z} $1"; }
hdr() { echo; echo "──────── $1 ────────"; }

# ══════════════════════════════════════════════════════════════════
# 传输层：把板端命令与发包动作抽象成两个函数，local / winrelay 两种实现
# ══════════════════════════════════════════════════════════════════
WIN_DIR=""
WIN_PY=""

setup_local() {
    command -v sshpass >/dev/null || { echo "缺 sshpass（或改用 --transport winrelay）" >&2; return 1; }
    command -v python3 >/dev/null || { echo "缺 python3" >&2; return 1; }
    # 板子直连可达性
    timeout 6 bash -c "exec 3<>/dev/tcp/$BOARD_IP/22" 2>/dev/null || return 1
    return 0
}

setup_winrelay() {
    command -v powershell.exe >/dev/null || return 1
    local wtmp
    wtmp=$(timeout 30 powershell.exe -NoProfile -Command 'Write-Output $env:TEMP' 2>/dev/null \
           | tr -d '\r' | tr '\\' '/')
    [ -n "$wtmp" ] || return 1
    WIN_DIR="/mnt/c${wtmp#C:}/ntpnic"
    WIN_PY=$(timeout 30 powershell.exe -NoProfile -Command '
        (Get-Command python -ErrorAction SilentlyContinue).Source' 2>/dev/null | tr -d '\r')
    [ -n "$WIN_PY" ] || return 1
    mkdir -p "$WIN_DIR" || return 1
    printf '@echo %s' "$BOARD_SSH_PASS" > "$WIN_DIR/askpass.cmd"
    cat > "$WIN_DIR/ntpflood.py" <<'PY'
import socket, sys
src, dst, count = sys.argv[1], sys.argv[2], int(sys.argv[3])
pkt = bytearray(48); pkt[0] = 0x1B          # LI=0 VN=3 Mode=3 (NTP client)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind((src, 0))
for _ in range(count):
    s.sendto(bytes(pkt), (dst, 123))
s.close(); print("SENT %d from %s to %s" % (count, src, dst))
PY
    cat > "$WIN_DIR/bssh.ps1" <<'PS1'
param([string]$B64File, [string]$Board)
Set-Location $env:TEMP
$env:SSH_ASKPASS = Join-Path $env:TEMP 'ntpnic\askpass.cmd'
$env:SSH_ASKPASS_REQUIRE = 'force'; $env:DISPLAY = 'localhost:0'
$b64 = (Get-Content -Raw -LiteralPath $B64File).Trim()
& ssh -T -q -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o ConnectTimeout=10 `
    -o LogLevel=ERROR root@$Board "echo $b64 | base64 -d | sh"
exit $LASTEXITCODE
PS1
    return 0
}

case "$TRANSPORT" in
    auto)
        if setup_local; then TRANSPORT=local
        elif setup_winrelay; then TRANSPORT=winrelay
        else echo "无法建立到 $BOARD_IP 的通道（local 与 winrelay 都失败）" >&2; exit 2; fi ;;
    local)    setup_local    || { echo "local 通道不可用" >&2; exit 2; } ;;
    winrelay) setup_winrelay || { echo "winrelay 通道不可用" >&2; exit 2; } ;;
    *) echo "--transport 只能是 auto|local|winrelay" >&2; exit 2 ;;
esac
info "传输层: $TRANSPORT"

# bsh: 在板端执行 stdin 里的脚本
bsh() {
    if [ "$TRANSPORT" = local ]; then
        sshpass -p "$BOARD_SSH_PASS" ssh -o StrictHostKeyChecking=no \
            -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 \
            "root@$BOARD_IP" 'sh' 2>&1
    else
        local b64
        b64=$(base64 -w0 | tr -d '\n')
        printf '%s' "$b64" > "$WIN_DIR/payload.b64"
        timeout 60 powershell.exe -NoProfile -ExecutionPolicy Bypass \
            -File "$(winpath "$WIN_DIR/bssh.ps1")" \
            -B64File "$(winpath "$WIN_DIR/payload.b64")" -Board "$BOARD_IP" 2>&1 | tr -d '\r'
    fi
}

winpath() { echo "C:${1#/mnt/c}" | tr '/' '\\'; }

# flood SRC DST COUNT: 从 PC 注入 COUNT 个 NTP client 请求，绑定 SRC 强制出口网卡
flood() {
    local src="$1" dst="$2" cnt="$3"
    if [ "$TRANSPORT" = local ]; then
        python3 -c "
import socket,sys
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind((sys.argv[1],0))
p=bytearray(48); p[0]=0x1B
for _ in range(int(sys.argv[3])): s.sendto(bytes(p),(sys.argv[2],123))
print('SENT',sys.argv[3],'from',sys.argv[1],'to',sys.argv[2])
" "$src" "$dst" "$cnt"
    else
        timeout 60 powershell.exe -NoProfile -Command \
            "& '$WIN_PY' '$(winpath "$WIN_DIR/ntpflood.py")' '$src' '$dst' '$cnt'" 2>&1 | tr -d '\r'
    fi
}

# 板端 HTTP：走 tunnel（winrelay）或直连
http_login() {
    if [ "$TRANSPORT" = local ]; then
        curl -sk -D - -o /dev/null -X POST --data-urlencode "user=$WEB_USER" \
             --data-urlencode "pass=$WEB_PASS" "https://$BOARD_IP/cgi-bin/login.cgi"
    else
        timeout 60 powershell.exe -NoProfile -Command \
            "& curl.exe -sk -D - -o NUL -X POST --data-urlencode 'user=$WEB_USER' --data-urlencode 'pass=$WEB_PASS' 'https://$BOARD_IP/cgi-bin/login.cgi'" 2>&1 | tr -d '\r'
    fi
}
http_get() {
    if [ "$TRANSPORT" = local ]; then
        curl -sk -b "session_id=$SID" "https://$BOARD_IP$1"
    else
        timeout 60 powershell.exe -NoProfile -Command \
            "& curl.exe -sk -H 'Cookie: session_id=$SID' 'https://$BOARD_IP$1'" 2>&1 | tr -d '\r'
    fi
}

# ══════════════════════════════════════════════════════════════════
# 板端采集
# ══════════════════════════════════════════════════════════════════

# 输出 4 行 "e0 <iptables> <rx_packets>"，一行 serverstats，一行最后 CSV 行
snapshot() {
    bsh <<'EOF'
for i in 0 1 2 3; do
    n=$(iptables -nvx -L INPUT 2>/dev/null | awk -v ifn="eth$i" \
        '$3=="ACCEPT" && $4=="udp" && $6==ifn && /udp dpt:123/ {print $1; exit}')
    printf 'e%s %s %s\n' "$i" "${n:-MISSING}" \
        "$(cat /sys/class/net/eth$i/statistics/rx_packets 2>/dev/null)"
done
echo "SS $(chronyc -c serverstats 2>/dev/null | tr -d '\r')"
echo "CSV $(tail -1 /var/db/ntp_stats.csv 2>/dev/null)"
EOF
}

# snap_get <snapshot-text> <key>  取 e0/e1/e2/e3 的 iptables 值
ipt_of() { echo "$1" | awk -v k="e$2" '$1==k {print $2}'; }
rx_of()  { echo "$1" | awk -v k="e$2" '$1==k {print $3}'; }
hits_of(){ echo "$1" | awk '$1=="SS" {split($2,a,","); print a[1]}'; }

# 等一个自然采样点（timer 每分钟 *:00 触发）
wait_sample() {
    local before after
    before=$(bsh <<'EOF'
wc -l < /var/db/ntp_stats.csv
EOF
)
    local i
    for i in $(seq 1 75); do
        sleep 1
        after=$(bsh <<'EOF'
wc -l < /var/db/ntp_stats.csv
EOF
)
        [ "${after:-0}" -gt "${before:-0}" ] 2>/dev/null && return 0
    done
    return 1
}

# ══════════════════════════════════════════════════════════════════
echo "========================================="
echo " 逐网口 NTP 流量监控测试 — $BOARD_IP"
echo " 传输层 $TRANSPORT | eth0 侧源 $SRC_ETH0 | eth1 侧源 $SRC_ETH1"
echo "========================================="

hdr "0. 前置检查"
PRE=$(snapshot)
if echo "$PRE" | grep -q MISSING; then
    bad "iptables 计数规则不全：$(echo "$PRE" | grep MISSING | tr '\n' ' ')"
    echo "     请先在板端跑一次 /usr/local/bin/ntp_stats_sample.sh" >&2
    exit 1
fi
ok "四个网口的计数规则均在位"
info "起始计数 e0=$(ipt_of "$PRE" 0) e1=$(ipt_of "$PRE" 1) e2=$(ipt_of "$PRE" 2) e3=$(ipt_of "$PRE" 3)"

TIMER=$(bsh <<'EOF'
systemctl is-active ntp-stats.timer 2>&1
EOF
)
[ "$TIMER" = active ] && ok "ntp-stats.timer 处于 active" || bad "ntp-stats.timer 未运行（$TIMER）"

if [ -z "$ETH1_IP" ]; then
    ETH1_IP=$(bsh <<'EOF'
ip -4 -br addr show eth1 2>/dev/null | awk '{print $3}' | cut -d/ -f1
EOF
)
fi
if [ -n "$ETH1_IP" ]; then ok "板端 eth1 地址: $ETH1_IP"; else bad "读不到板端 eth1 的 IPv4 地址"; exit 1; fi

# 登录 Web（CGI JSON 断言用）
LOGIN=$(http_login)
SID=$(echo "$LOGIN" | grep -o 'session_id=[^;]*' | head -1 | cut -d= -f2)
[ -n "$SID" ] && ok "Web 登录成功（$WEB_USER）" || bad "Web 登录失败，跳过 JSON 断言"

hdr "1. 路由归属探针（先证伪，再断言）"
# 用物理收包计数确认"绑定源地址"真的把包送到了目标网口
probe() { # probe <src> <dst> <n> <expect_nic>
    local src="$1" dst="$2" n="$3" want="$4"
    local a b ra rb
    a=$(snapshot); flood "$src" "$dst" "$n" >/dev/null; sleep 1; b=$(snapshot)
    local d0 d1
    d0=$(( $(ipt_of "$b" 0) - $(ipt_of "$a" 0) ))
    d1=$(( $(ipt_of "$b" 1) - $(ipt_of "$a" 1) ))
    ra=$(( $(rx_of "$b" 0) - $(rx_of "$a" 0) ))
    rb=$(( $(rx_of "$b" 1) - $(rx_of "$a" 1) ))
    info "src=$src → dst=$dst : iptables e0+$d0 e1+$d1 | rx eth0+$ra eth1+$rb"
    if [ "$want" = 0 ]; then
        [ "$d0" -eq "$n" ] && [ "$d1" -eq 0 ] && ok "归属正确：$n 包全部落在 eth0" \
            || bad "归属异常：期望 e0+$n e1+0，实得 e0+$d0 e1+$d1"
    else
        [ "$d1" -eq "$n" ] && [ "$d0" -eq 0 ] && ok "归属正确：$n 包全部落在 eth1" \
            || bad "归属异常：期望 e1+$n e0+0，实得 e1+$d1 e0+$d0"
    fi
}
probe "$SRC_ETH0" "$BOARD_IP" 3 0
probe "$SRC_ETH1" "$ETH1_IP" 3 1

if [ "$FAIL" -gt 0 ]; then
    echo; echo "${R}归属探针失败，拓扑不满足逐网口测试前提，终止。${Z}"
    echo "常见原因：两块 PC 网卡处于同一网段导致路由二选一；或源地址绑定的不是该链路的地址。" >&2
    exit 1
fi

hdr "2. 静默基线（无注入时计数不应变化）"
if [ "$QUICK" = 1 ]; then info "已跳过（--quick）"; else
    A=$(snapshot); sleep 70; B=$(snapshot)
    d0=$(( $(ipt_of "$B" 0) - $(ipt_of "$A" 0) )); d1=$(( $(ipt_of "$B" 1) - $(ipt_of "$A" 1) ))
    d2=$(( $(ipt_of "$B" 2) - $(ipt_of "$A" 2) )); d3=$(( $(ipt_of "$B" 3) - $(ipt_of "$A" 3) ))
    [ "$d0" -eq 0 ] && [ "$d1" -eq 0 ] && [ "$d2" -eq 0 ] && [ "$d3" -eq 0 ] \
        && ok "70 秒内四口计数零变化（噪声底为 0）" \
        || bad "存在背景 NTP 流量：e0+$d0 e1+$d1 e2+$d2 e3+$d3（断言需按增量而非绝对值）"
fi

hdr "3. eth0 单口注入 $N1 包"
A=$(snapshot); flood "$SRC_ETH0" "$BOARD_IP" "$N1"; sleep 2; B=$(snapshot)
d0=$(( $(ipt_of "$B" 0) - $(ipt_of "$A" 0) )); d1=$(( $(ipt_of "$B" 1) - $(ipt_of "$A" 1) ))
d2=$(( $(ipt_of "$B" 2) - $(ipt_of "$A" 2) )); d3=$(( $(ipt_of "$B" 3) - $(ipt_of "$A" 3) ))
dh=$(( $(hits_of "$B") - $(hits_of "$A") ))
ra=$(( $(rx_of "$B" 0) - $(rx_of "$A" 0) )); rb=$(( $(rx_of "$B" 1) - $(rx_of "$A" 1) ))
[ "$d0" -eq "$N1" ] && ok "iptables e0 +$N1（实得 +$d0）" || bad "iptables e0 期望 +$N1，实得 +$d0"
[ "$d1" -eq 0 ] && [ "$d2" -eq 0 ] && [ "$d3" -eq 0 ] \
    && ok "eth1/eth2/eth3 计数无串扰（+$d1/+$d2/+$d3）" \
    || bad "串扰！eth1+$d1 eth2+$d2 eth3+$d3"
# 活跃链路上 rx_packets 含背景流量（ARP/邻居发现等），不能断言绝对零增长；
# 精确的归属证明由第 1 节的紧窗口探针给出，这里只断言注入量没有整体落到 eth1。
[ "$rb" -lt $((N1 / 2)) ] && ok "物理层交叉验证：eth1 收包仅 +$rb（未被 $N1 个注入包波及）" \
    || bad "eth1 收包 +$rb，接近注入量 $N1，疑似归属错误"
info "eth0 收包 +$ra（含背景流量），ntp_hits +$dh"
E0_AFTER=$(ipt_of "$B" 0); E1_AFTER=$(ipt_of "$B" 1)

hdr "4. eth1 单口注入 $N2 包"
A=$(snapshot); flood "$SRC_ETH1" "$ETH1_IP" "$N2"; sleep 2; B=$(snapshot)
d0=$(( $(ipt_of "$B" 0) - $(ipt_of "$A" 0) )); d1=$(( $(ipt_of "$B" 1) - $(ipt_of "$A" 1) ))
dh=$(( $(hits_of "$B") - $(hits_of "$A") ))
rb=$(( $(rx_of "$B" 1) - $(rx_of "$A" 1) ))
[ "$d1" -eq "$N2" ] && ok "iptables e1 +$N2（实得 +$d1）" || bad "iptables e1 期望 +$N2，实得 +$d1"
[ "$d0" -eq 0 ] && ok "eth0 计数无串扰（+$d0）" || bad "串扰！eth0+$d0"
[ "$rb" -ge "$N2" ] && ok "物理层交叉验证：eth1 收包 +$rb（含 $N2 个注入包与背景流量）" \
    || bad "eth1 收包仅 +$rb，少于注入的 $N2 个包"
info "ntp_hits +$dh（若为 0 说明该源被 chrony ACL 拒绝 —— 见 docs/ntp-monitor.md）"
E1_AFTER=$(ipt_of "$B" 1)

hdr "5. 两侧并发注入（各 $N1 包）"
A=$(snapshot)
flood "$SRC_ETH0" "$BOARD_IP" "$N1" >/dev/null &
P1=$!
flood "$SRC_ETH1" "$ETH1_IP" "$N1" >/dev/null &
P2=$!
wait $P1 $P2; sleep 2; B=$(snapshot)
d0=$(( $(ipt_of "$B" 0) - $(ipt_of "$A" 0) )); d1=$(( $(ipt_of "$B" 1) - $(ipt_of "$A" 1) ))
[ "$d0" -eq "$N1" ] && [ "$d1" -eq "$N1" ] \
    && ok "并发下两口径各自独立归属（e0 +$d0 / e1 +$d1）" \
    || bad "并发归属异常：期望 +$N1/+$N1，实得 e0+$d0 e1+$d1"
E0_AFTER=$(ipt_of "$B" 0); E1_AFTER=$(ipt_of "$B" 1)

hdr "6. 自然采样落盘 + CGI JSON 一致性"
if wait_sample; then
    CSV=$(bsh <<'EOF'
tail -1 /var/db/ntp_stats.csv
EOF
)
    info "新采样行: $CSV"
    c_e0=$(echo "$CSV" | awk -F, '{print $7}'); c_e1=$(echo "$CSV" | awk -F, '{print $8}')
    c_e2=$(echo "$CSV" | awk -F, '{print $9}'); c_e3=$(echo "$CSV" | awk -F, '{print $10}')
    [ "$c_e0" -ge "$E0_AFTER" ] 2>/dev/null && [ "$c_e1" -ge "$E1_AFTER" ] 2>/dev/null \
        && ok "CSV 落盘值不低于注入后实测计数（e0=$c_e0 e1=$c_e1）" \
        || bad "CSV 与实测计数不一致：CSV e0=$c_e0 e1=$c_e1，实测 e0=$E0_AFTER e1=$E1_AFTER"
    [ "$c_e2" -eq 0 ] && [ "$c_e3" -eq 0 ] && ok "未接线网口 eth2/eth3 列恒为 0" \
        || bad "未接线网口出现非零计数：e2=$c_e2 e3=$c_e3"
else
    bad "75 秒内未见新的采样行（采样器可能未运行）"
fi

if [ -n "$SID" ]; then
    J=$(http_get "/cgi-bin/ntpmon.cgi?action=stats")
    J_E0=$(echo "$J" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['eth']['total'][0])" 2>/dev/null)
    J_E1=$(echo "$J" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['eth']['total'][1])" 2>/dev/null)
    J_AGE=$(echo "$J" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['meta']['csv_age_s'])" 2>/dev/null)
    J_NAMES=$(echo "$J" | python3 -c "import json,sys; d=json.load(sys.stdin); print(','.join(d['eth']['names']))" 2>/dev/null)
    [ "$J_NAMES" = "eth0,eth1,eth2,eth3" ] && ok "JSON eth.names 为 eth0..eth3" || bad "eth.names 异常: $J_NAMES"
    [ -n "$J_E0" ] && [ "$J_E0" -ge "$E0_AFTER" ] 2>/dev/null \
        && ok "JSON eth.total[0]=$J_E0 ≥ 实测 $E0_AFTER" || bad "JSON eth.total[0]=$J_E0 与实测 $E0_AFTER 不符"
    [ -n "$J_E1" ] && [ "$J_E1" -ge "$E1_AFTER" ] 2>/dev/null \
        && ok "JSON eth.total[1]=$J_E1 ≥ 实测 $E1_AFTER" || bad "JSON eth.total[1]=$J_E1 与实测 $E1_AFTER 不符"
    [ -n "$J_AGE" ] && [ "$J_AGE" -lt 180 ] 2>/dev/null \
        && ok "JSON meta.csv_age_s=$J_AGE < 180（采样器新鲜）" || bad "csv_age_s=$J_AGE 偏大"

    # ── 7. per-interface serving detail (ntp_nic_monitor daemon) ──
    hdr "7. 逐网口授时服务状态与客户端明细"
    if [ "$(echo "$J" | python3 -c "import json,sys; print(json.load(sys.stdin)['nic']['ok'])" 2>/dev/null)" != "True" ]; then
        info "采集进程未运行（ntp-nic-monitor.service），跳过本节"
    else
        eval "$(echo "$J" | python3 -c "
import json,sys
d=json.load(sys.stdin)['nic']
print('NIC_AGE=%s' % d.get('age_s'))
for n in d['nics']:
    print('NIC_%s_REQ=%d' % (n['name'], n['req']))
    print('NIC_%s_RSP=%d' % (n['name'], n['rsp']))
    print('NIC_%s_CLI=%d' % (n['name'], len(d['clients'].get(n['name'], []))))
c0 = d['clients'].get('eth0', [])
print('NIC_ETH0_FIRST_IP=%s' % (c0[0]['ip'] if c0 else ''))
print('NIC_ETH0_FIRST_REQ=%s' % (c0[0]['req'] if c0 else 0))
" 2>/dev/null)"

        [ "${NIC_AGE:-999}" -lt 30 ] 2>/dev/null \
            && ok "采集快照新鲜（age_s=$NIC_AGE）" || bad "采集快照过期（age_s=${NIC_AGE:-?}）"
        [ "${NIC_eth0_REQ:-0}" -ge "$N1" ] 2>/dev/null \
            && ok "客户端侧 eth0 请求计数 ${NIC_eth0_REQ} ≥ 注入 $N1" \
            || bad "eth0 请求计数 ${NIC_eth0_REQ:-0} < 注入 $N1"
        [ "${NIC_eth1_REQ:-0}" -ge "$N2" ] 2>/dev/null \
            && ok "客户端侧 eth1 请求计数 ${NIC_eth1_REQ} ≥ 注入 $N2" \
            || bad "eth1 请求计数 ${NIC_eth1_REQ:-0} < 注入 $N2"
        [ "${NIC_eth2_REQ:-9}" -eq 0 ] && [ "${NIC_eth3_REQ:-9}" -eq 0 ] \
            && ok "未接线网口无客户端流量" || bad "eth2/eth3 出现客户端流量"
        [ "$NIC_ETH0_FIRST_IP" = "$SRC_ETH0" ] \
            && ok "客户端明细归属正确：eth0 ← $SRC_ETH0" \
            || bad "eth0 的首个客户端为 ${NIC_ETH0_FIRST_IP:-空}，期望 $SRC_ETH0"
        [ "${NIC_eth0_RSP:-0}" -gt 0 ] && [ "${NIC_eth0_REQ:-0}" -gt 0 ] 2>/dev/null \
            && ok "应答量已统计（eth0 rsp=$NIC_eth0_RSP / req=$NIC_eth0_REQ）" \
            || bad "eth0 应答量为 0（iptables OUTPUT 规则缺失？）"
    fi
fi

hdr "结果"
echo "  PASS=$PASS  FAIL=$FAIL"
[ "$FAIL" -eq 0 ] && { echo "  ${G}全部通过${Z}"; exit 0; } || { echo "  ${R}存在失败项${Z}"; exit 1; }

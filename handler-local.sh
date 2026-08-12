#!/bin/sh
# handler-local.sh — LubanCat 本机串口协议执行器 (ADR-0001)
# 不监听串口,而是监听 FIFO;收到 REMOTE_GET/SET 命令后用 nmcli
# (connection profile) 持久化地修改本机网络,并带看门狗回滚:
# 新 IP 在 WATCHDOG_SECS 内未被 REMOTE_CONFIRM_IPV4 确认则自动回滚。
# 复用 handler.sh 的 flock / PID / 重启框架。
#
# 部署:
#   scp handler-local.sh root@<board>:/usr/local/bin/
#   ssh root@<board> 'chmod +x /usr/local/bin/handler-local.sh; \
#     rm -f /var/run/serial_protocol.lock /var/run/serial_protocol.pid; \
#     nohup /usr/local/bin/handler-local.sh > /tmp/handler-local.log 2>&1 &'

FIFO=/run/serial_protocol.fifo
RESP=/run/serial_protocol.resp
LOG=/var/log/serial_protocol.log
IFACE=eth0
LOCK=/var/run/serial_protocol.lock
PIDFILE=/var/run/serial_protocol.pid
ROLLBACK=/var/run/serial_protocol.rollback
WATCHDOG_PIDFILE=/var/run/serial_protocol.watchdog
WATCHDOG_SECS=180
WATCHDOG_GEN=0

log() { echo "$(date '+%F %T') $*" >> $LOG; }

log "Local handler starting on FIFO $FIFO (resp: $RESP)"

# ── Clean up stale lock from previous crashed instance ──────────
if [ -f "$PIDFILE" ]; then
    OLD_PID=$(cat "$PIDFILE" 2>/dev/null)
    if [ -n "$OLD_PID" ] && ! kill -0 "$OLD_PID" 2>/dev/null; then
        log "Removing stale PID file (PID $OLD_PID no longer running)"
        rm -f "$LOCK" "$PIDFILE" "$ROLLBACK" "$WATCHDOG_PIDFILE"
    fi
fi

# ── Acquire exclusive lock ─────────────────────────────────────
exec 9>"$LOCK"
if ! flock -n 9; then
    log "Another handler is running. Exiting."
    exit 1
fi
echo $$ > "$PIDFILE"
log "Lock acquired (PID $$)"

# ── Trap cleanup ───────────────────────────────────────────────
cleanup() {
    log "Handler exiting (signal)"
    rm -f "$LOCK" "$PIDFILE" "$ROLLBACK" "$WATCHDOG_PIDFILE"
    exit 0
}
trap cleanup INT TERM

# ── nmcli helpers ──────────────────────────────────────────────
# NetworkManager 连接名(profile 修改才能持久化,勿用 dev modify)
conn_name() {
    nmcli -t -f GENERAL.CONNECTION dev show "$IFACE" 2>/dev/null | cut -d: -f2-
}

# 应用 IPv4 配置(persistent)。$1=ip $2=cidr $3=gw(0.0.0.0 视为无网关)
apply_ipv4() {
    local ip="$1" cidr="$2" gw="$3"
    local con
    con=$(conn_name)
    if [ -z "$con" ]; then
        log "ERROR: no NetworkManager connection on $IFACE"
        return 1
    fi
    if [ "$gw" = "0.0.0.0" ]; then gw=""; fi
    if [ -n "$gw" ]; then
        nmcli con mod "$con" ipv4.method manual ipv4.addresses "$ip/$cidr" \
              ipv4.gateway "$gw" 2>>$LOG || return 1
    else
        nmcli con mod "$con" ipv4.method manual ipv4.addresses "$ip/$cidr" \
              ipv4.gateway "" 2>>$LOG || return 1
    fi
    nmcli con up "$con" 2>>$LOG || return 1
    return 0
}

# ── Watchdog rollback ──────────────────────────────────────────
# 仅当自己是最新登记的看门狗(代数一致)且新 IP 仍生效时才回滚,
# 避免旧看门狗误回滚后续的配置修改。
# 注:dash 子 shell 中 $$ 仍是主 shell PID,故用代数标记而非 pid 自检。
watchdog_rollback() {
    [ "$(awk '{print $1}' "$WATCHDOG_PIDFILE" 2>/dev/null)" = "$1" ] || return 0
    [ -f "$ROLLBACK" ] || return 0
    read -r NEW_IP NEW_CIDR NEW_GW OLD_IP OLD_CIDR OLD_GW < "$ROLLBACK"
    [ -n "$OLD_IP" ] || return 0
    CUR=$(ip -4 -o addr show "$IFACE" 2>/dev/null | awk '{print $4}' | head -1 | cut -d/ -f1)
    [ "$CUR" = "$NEW_IP" ] || return 0
    if apply_ipv4 "$OLD_IP" "$OLD_CIDR" "$OLD_GW"; then
        log "Watchdog: rolled back to $OLD_IP/$OLD_CIDR"
    else
        log "Watchdog: rollback FAILED"
    fi
    rm -f "$ROLLBACK" "$WATCHDOG_PIDFILE"
}

# 取消看门狗(REMOTE_CONFIRM_IPV4)
cancel_watchdog() {
    rm -f "$ROLLBACK"
    if [ -f "$WATCHDOG_PIDFILE" ]; then
        kill "$(awk '{print $2}' "$WATCHDOG_PIDFILE")" 2>/dev/null
        rm -f "$WATCHDOG_PIDFILE"
    fi
}

# ── Ensure FIFOs exist and are world-writable (CGI 以 www-data 访问) ──
[ -p "$FIFO" ] || { mkfifo "$FIFO" 2>/dev/null || true; }
[ -p "$RESP" ] || { mkfifo "$RESP" 2>/dev/null || true; }
chmod 666 "$FIFO" "$RESP" 2>/dev/null

# 响应通道:handler 只写 resp(9),只读 cmd(8)——双 FIFO 避免自我回显
exec 9<>"$RESP" 2>/dev/null
if [ $? -ne 0 ]; then
    log "ERROR: Cannot open resp FIFO $RESP"
fi

# ── Main loop: reopen FIFO if it dies ───────────────────────────
while true; do
    if ! exec 8<>"$FIFO" 2>/dev/null; then
        log "ERROR: Cannot open $FIFO, retrying in 3s..."
        sleep 3
        continue
    fi

    log "Entering command loop"

    while read -r line <&8; do
        log "RX: $line"

        case "$line" in
            PING)
                echo "OK" >&9
                log "TX: OK"
                ;;

            REMOTE_GET_IPV4)
                IP=$(ip -4 -o addr show $IFACE 2>/dev/null | awk '{print $4}' | head -1 | cut -d/ -f1)
                MASK=$(ip -4 -o addr show $IFACE 2>/dev/null | awk '{print $4}' | head -1 | cut -d/ -f2)
                GW=$(ip -4 route show default 2>/dev/null | awk '{print $3}' | head -1)

                # Convert CIDR prefix to netmask
                case "$MASK" in
                    24) NM=255.255.255.0 ;; 16) NM=255.255.0.0 ;; 8) NM=255.0.0.0 ;;
                    25) NM=255.255.255.128 ;; 26) NM=255.255.255.192 ;;
                    27) NM=255.255.255.224 ;; 28) NM=255.255.255.240 ;;
                    29) NM=255.255.255.248 ;; 30) NM=255.255.255.252 ;;
                    32) NM=255.255.255.255 ;;  *) NM=255.255.255.0 ;;
                esac
                [ -z "$IP" ] && IP=0.0.0.0
                [ -z "$GW" ] && GW=0.0.0.0

                echo "OK $IP $NM $GW" >&9
                log "TX: OK $IP $NM $GW"
                ;;

            REMOTE_SET_IPV4\ *)
                ARGS="${line#REMOTE_SET_IPV4 }"
                NEW_IP=$(echo "$ARGS" | awk '{print $1}')
                NEW_MASK=$(echo "$ARGS" | awk '{print $2}')
                NEW_GW=$(echo "$ARGS" | awk '{print $3}')

                # Validate
                if echo "$NEW_IP" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' && \
                   echo "$NEW_MASK" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' && \
                   echo "$NEW_GW" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$'; then

                    # Convert netmask to CIDR
                    CIDR=24
                    case "$NEW_MASK" in
                        255.255.255.0) CIDR=24 ;; 255.255.0.0) CIDR=16 ;;
                        255.0.0.0) CIDR=8 ;; 255.255.255.128) CIDR=25 ;;
                        255.255.255.192) CIDR=26 ;; 255.255.255.224) CIDR=27 ;;
                        255.255.255.240) CIDR=28 ;; 255.255.255.248) CIDR=29 ;;
                        255.255.255.252) CIDR=30 ;; 255.255.255.255) CIDR=32 ;;
                    esac

                    # No-op detection: 与当前配置一致则不动 nmcli,避免无谓断连
                    CUR_IP=$(ip -4 -o addr show $IFACE 2>/dev/null | awk '{print $4}' | head -1 | cut -d/ -f1)
                    CUR_CIDR=$(ip -4 -o addr show $IFACE 2>/dev/null | awk '{print $4}' | head -1 | cut -d/ -f2)
                    CUR_GW=$(ip -4 route show default 2>/dev/null | awk '{print $3}' | head -1)

                    if [ "$CUR_IP" = "$NEW_IP" ] && [ "$CUR_CIDR" = "$CIDR" ] && \
                       [ "$CUR_GW" = "$NEW_GW" ]; then
                        echo "OK" >&9
                        log "TX: OK (IPv4 unchanged: $NEW_IP/$CIDR)"
                    else
                        OLD_IP="$CUR_IP"; OLD_CIDR="$CUR_CIDR"; OLD_GW="$CUR_GW"
                        [ -z "$OLD_IP" ] && OLD_IP=0.0.0.0
                        [ -z "$OLD_CIDR" ] && OLD_CIDR=24
                        [ -z "$OLD_GW" ] && OLD_GW=0.0.0.0

                        if apply_ipv4 "$NEW_IP" "$CIDR" "$NEW_GW"; then
                            # 存档旧配置并登记新看门狗(先淘汰旧看门狗)
                            echo "$NEW_IP $CIDR $NEW_GW $OLD_IP $OLD_CIDR $OLD_GW" > "$ROLLBACK"
                            if [ -f "$WATCHDOG_PIDFILE" ]; then
                                kill "$(awk '{print $2}' "$WATCHDOG_PIDFILE")" 2>/dev/null
                                rm -f "$WATCHDOG_PIDFILE"
                            fi
                            WATCHDOG_GEN=$((WATCHDOG_GEN + 1))
                            ( sleep "$WATCHDOG_SECS"; watchdog_rollback "$WATCHDOG_GEN" ) &
                            echo "$WATCHDOG_GEN $!" > "$WATCHDOG_PIDFILE"
                            echo "OK" >&9
                            log "TX: OK (IPv4 set: $NEW_IP/$CIDR via $NEW_GW, watchdog ${WATCHDOG_SECS}s)"
                        else
                            rm -f "$ROLLBACK"
                            echo "ERR nmcli failed" >&9
                            log "TX: ERR nmcli failed"
                        fi
                    fi
                else
                    echo "ERR Invalid IP address" >&9
                    log "TX: ERR Invalid IP address"
                fi
                ;;

            REMOTE_GET_IPV6)
                V6=$(ip -6 -o addr show $IFACE 2>/dev/null | grep -v fe80 | awk '{print $4}' | head -1)
                [ -z "$V6" ] && V6=$(ip -6 -o addr show $IFACE 2>/dev/null | awk '{print $4}' | head -1)
                [ -z "$V6" ] && V6="::/64"
                echo "OK $V6" >&9
                log "TX: OK $V6"
                ;;

            REMOTE_SET_IPV6\ *)
                V6ADDR="${line#REMOTE_SET_IPV6 }"
                CUR_V6=$(ip -6 -o addr show $IFACE 2>/dev/null | grep -v fe80 | awk '{print $4}' | head -1)
                if [ "$CUR_V6" = "$V6ADDR" ]; then
                    echo "OK" >&9
                    log "TX: OK (IPv6 unchanged: $V6ADDR)"
                else
                    CON=$(conn_name)
                    if [ -z "$CON" ]; then
                        echo "ERR no connection" >&9
                        log "TX: ERR no connection"
                    elif nmcli con mod "$CON" ipv6.method manual ipv6.addresses "$V6ADDR" 2>>$LOG && \
                         nmcli con up "$CON" 2>>$LOG; then
                        echo "OK" >&9
                        log "TX: OK (IPv6 set: $V6ADDR)"
                    else
                        echo "ERR Failed to set IPv6" >&9
                        log "TX: ERR Failed to set IPv6"
                    fi
                fi
                ;;

            REMOTE_CONFIRM_IPV4)
                cancel_watchdog
                echo "OK" >&9
                log "TX: OK (watchdog cancelled)"
                ;;

            *)
                echo "ERR Unknown command: ${line%% *}" >&9
                log "TX: ERR Unknown command: ${line%% *}"
                ;;
        esac
    done

    log "FIFO read exited, reopening in 2s..."
    exec 8>&- 2>/dev/null
    sleep 2
done

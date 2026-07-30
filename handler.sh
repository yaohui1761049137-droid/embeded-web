#!/bin/sh
# Minimal serial protocol handler — shell-based, can't crash
# Listens on DEV, responds to REMOTE_GET/SET commands.
# Auto-restarts serial loop on errors; cleans stale PID on startup.

DEV=/dev/ttyS7
LOG=/var/log/serial_protocol.log
IFACE=eth0
BAUD=115200
LOCK=/var/run/serial_protocol.lock
PIDFILE=/var/run/serial_protocol.pid

log() { echo "$(date '+%F %T') $*" >> $LOG; }

log "Shell handler starting on $DEV ($BAUD)"

# ── Clean up stale lock from previous crashed instance ──────────
if [ -f "$PIDFILE" ]; then
    OLD_PID=$(cat "$PIDFILE" 2>/dev/null)
    if [ -n "$OLD_PID" ] && ! kill -0 "$OLD_PID" 2>/dev/null; then
        log "Removing stale PID file (PID $OLD_PID no longer running)"
        rm -f "$LOCK" "$PIDFILE"
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
    exec 8>&- 2>/dev/null
    rm -f "$LOCK" "$PIDFILE"
    exit 0
}
trap cleanup INT TERM

# ── Main loop: restart serial if it dies ────────────────────────
while true; do
    log "Opening serial port $DEV ($BAUD)"

    # Configure serial port
    stty -F "$DEV" $BAUD cs8 -cstopb -parenb -echo -icanon -opost min 1 time 20 2>/dev/null

    # Open for read/write
    if ! exec 8<>"$DEV" 2>/dev/null; then
        log "ERROR: Cannot open $DEV, retrying in 3s..."
        sleep 3
        continue
    fi

    log "Entering command loop"

    while read -r line <&8; do
        log "RX: $line"

        case "$line" in
            PING)
                echo "OK" >&8
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

                echo "OK $IP $NM $GW" >&8
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

                    ip addr flush dev $IFACE 2>/dev/null
                    ip addr add $NEW_IP/$CIDR dev $IFACE
                    ip route del default 2>/dev/null
                    ip route add default via $NEW_GW 2>/dev/null
                    echo "OK" >&8
                    log "TX: OK (IPv4 set: $NEW_IP/$CIDR via $NEW_GW)"
                else
                    echo "ERR Invalid IP address" >&8
                    log "TX: ERR Invalid IP address"
                fi
                ;;

            REMOTE_GET_IPV6)
                V6=$(ip -6 -o addr show $IFACE 2>/dev/null | grep -v fe80 | awk '{print $4}' | head -1)
                [ -z "$V6" ] && V6=$(ip -6 -o addr show $IFACE 2>/dev/null | awk '{print $4}' | head -1)
                [ -z "$V6" ] && V6="::/64"
                echo "OK $V6" >&8
                log "TX: OK $V6"
                ;;

            REMOTE_SET_IPV6\ *)
                V6ADDR="${line#REMOTE_SET_IPV6 }"
                ip -6 addr flush dev $IFACE scope global 2>/dev/null
                if ip -6 addr add $V6ADDR dev $IFACE 2>/dev/null; then
                    echo "OK" >&8
                    log "TX: OK (IPv6 set: $V6ADDR)"
                else
                    echo "ERR Failed to set IPv6" >&8
                    log "TX: ERR Failed to set IPv6"
                fi
                ;;

            *)
                echo "ERR Unknown command: ${line%% *}" >&8
                log "TX: ERR Unknown command: ${line%% *}"
                ;;
        esac
    done

    log "Serial loop exited (read error or hangup), restarting in 2s..."
    exec 8>&- 2>/dev/null
    sleep 2
done

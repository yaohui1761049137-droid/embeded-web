#!/bin/sh
# pps_tod_watchdog.sh - PPS+TOD sync-chain liveness watchdog
# Trigger: the daemon's per-second status file reports no GOOD sample for
# GOOD_WINDOW seconds, or the status file goes stale (daemon dead).
# Action: systemctl restart pps_tod (its bootstrap re-locks the clock).
# The watchdog never writes the clock itself (that is the daemon's job, to
# avoid fighting chrony) and never restarts on reference loss - it escalates
# to DEGRADED + periodic alert instead, because a restart cannot re-connect
# a missing PPS/TOD signal. Anti-storm: cooldown + max restarts per hour.
#
# Log retention is built in (no logrotate dependency): watchdog.log is a
# single file, so when it exceeds LOG_MAX_KB the OLDEST lines are dropped and
# the newest LOG_KEEP_LINES kept.  A persistent reference loss writes one
# alert a minute (~230 KB/day), so without a cap this file grows without
# bound.  `--prune-only` applies the cap once and exits (also used by tests).
# Config path is overridable for the same reason the paths below are: the
# prune logic must be testable without the board's /etc.  Note a missing conf
# kills a non-interactive sh ('.' is a special builtin), so the default must
# exist in production.
. "${PPS_TOD_WATCHDOG_CONF:-/etc/pps_tod/pps_tod_watchdog.conf}"

# Paths are env-overridable so the prune logic can be driven from a test
# harness without touching the live files (same convention as the repo's
# rollback_watchdog.sh: ROLLBACK_FILE / AUTHLOG).
STATUS=${PPS_TOD_STATUS:-/run/pps_tod/status}
STATE=${PPS_TOD_STATE:-/run/pps_tod/watchdog.state}
LOG=${PPS_TOD_WATCHDOG_LOG:-/var/log/pps_tod/watchdog.log}

mkdir -p /run/pps_tod /var/log/pps_tod

log() {
    line="$(date '+%F %T') WATCHDOG: $*"
    echo "$line" >> "$LOG" 2>/dev/null
    echo "$line"
}

last_prune=0
last_trim_logged=0
# Capacity cap for watchdog.log.  Keeps the newest lines; the file is the
# only record of what the watchdog did, so dropping the head (oldest events)
# is the right end to lose.
prune_log() {
    [ "${LOG_MAX_KB:-0}" -gt 0 ] 2>/dev/null || return 0
    size=$(stat -c %s "$LOG" 2>/dev/null) || return 0
    [ "$size" -gt $((LOG_MAX_KB * 1024)) ] || return 0
    tmp="$LOG.tmp"
    tail -n "${LOG_KEEP_LINES:-2000}" "$LOG" > "$tmp" 2>/dev/null || { rm -f "$tmp"; return 0; }
    mv "$tmp" "$LOG" 2>/dev/null || { rm -f "$tmp"; return 0; }
    # Throttled: a cap smaller than LOG_KEEP_LINES can never be satisfied, and
    # without this it would append a "trimmed" line on every pass.
    if [ $(( $(date +%s) - last_trim_logged )) -ge 3600 ]; then
        last_trim_logged=$(date +%s)
        log "watchdog.log over ${LOG_MAX_KB}KB -> trimmed to the last ${LOG_KEEP_LINES:-2000} lines"
    fi
}

start_epoch=$(date +%s)
mode=OK
bad_since=0
last_action=0
last_alert=0
observe_until=0
strikes=""

is_good() {
    [ -f "$STATUS" ] || return 1
    now=$(date +%s)
    mt=$(stat -c %Y "$STATUS" 2>/dev/null) || return 1
    [ $((now - mt)) -le "$STALE_MAX" ] || return 1
    age=$(sed -n 's/^last_good_age_s=//p' "$STATUS")
    case "$age" in ''|*[!0-9]*) return 1;; esac
    [ "$age" -le "$GOOD_WINDOW" ] || return 1
    return 0
}

write_state() {
    {
        echo "ts=$(date '+%F %T')"
        echo "mode=$mode"
        echo "bad_since=$bad_since"
        echo "last_action=$last_action"
        echo "observe_until=$observe_until"
        echo "strikes=$strikes"
    } > "$STATE.tmp" 2>/dev/null && mv "$STATE.tmp" "$STATE" 2>/dev/null
}

# Operational/test entry point: apply the cap once and exit.  Keeps prune_log
# reachable without waiting for the loop, and lets a test drive it directly.
if [ "${1:-}" = "--prune-only" ]; then
    prune_log
    echo "prune done (cap=${LOG_MAX_KB:-0}KB keep=${LOG_KEEP_LINES:-2000} lines, now $(wc -l < "$LOG" 2>/dev/null || echo 0) lines)"
    exit 0
fi

log "watchdog started (GOOD_WINDOW=${GOOD_WINDOW}s cooldown=${RESTART_COOLDOWN}s max=${MAX_RESTARTS}/h log_cap=${LOG_MAX_KB:-0}KB keep=${LOG_KEEP_LINES:-2000})"

while :; do
    now=$(date +%s)
    if [ $((now - last_prune)) -ge 60 ]; then
        last_prune=$now
        prune_log
    fi
    if [ $((now - start_epoch)) -lt "$START_GRACE" ]; then
        write_state
        sleep 1
        continue
    fi
    if is_good; then
        case "$mode" in
        RECOVERING|DEGRADED)
            log "recovered: GOOD samples flowing again (was $mode)"
            ;;
        esac
        mode=OK
        bad_since=0
        observe_until=0
    else
        case "$mode" in
        OK)
            [ "$bad_since" = 0 ] && bad_since=$now
            if [ $((now - bad_since)) -ge "$GOOD_WINDOW" ]; then
                if [ $((now - last_action)) -lt "$RESTART_COOLDOWN" ]; then
                    :  # within cooldown: keep observing
                else
                    fresh=""
                    for s in $strikes; do
                        [ $((now - s)) -lt 3600 ] && fresh="$fresh $s"
                    done
                    strikes=$fresh
                    n=$(set -- $strikes; echo $#)
                    if [ "$n" -ge "$MAX_RESTARTS" ]; then
                        log "MAX_RESTARTS ($n within 1h) -> DEGRADED, auto-restart disabled"
                        mode=DEGRADED
                    else
                        log "no GOOD sample for ${GOOD_WINDOW}s -> restart pps_tod"
                        systemctl restart pps_tod
                        rc=$?
                        strikes="$strikes $now"
                        last_action=$now
                        observe_until=$((now + OBSERVE_GRACE))
                        if [ "$rc" -ne 0 ]; then
                            log "systemctl restart FAILED rc=$rc -> DEGRADED"
                            mode=DEGRADED
                        else
                            mode=RECOVERING
                        fi
                    fi
                fi
            fi
            ;;
        RECOVERING)
            if [ "$now" -ge "$observe_until" ]; then
                log "no GOOD within ${OBSERVE_GRACE}s after restart -> DEGRADED (reference lost? wiring?)"
                mode=DEGRADED
            fi
            ;;
        DEGRADED)
            if [ $((now - last_alert)) -ge "$ALERT_INTERVAL" ]; then
                last_alert=$now
                log "DEGRADED: still no GOOD sample; check PPS/TOD wiring or receiver (restart will not help). Manual reset: systemctl restart pps_tod-watchdog"
            fi
            ;;
        esac
    fi
    write_state
    sleep 1
done
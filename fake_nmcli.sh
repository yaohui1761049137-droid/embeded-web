#!/bin/bash
# Fake nmcli for offline host tests (NMCLI_OVERRIDE).
# State: FAKE_NMCLI_STATE dir (default /tmp/nmtest/state) holding:
#   profiles   — "name|uuid|ifname|ipv4addr|gateway|dns|ipv6addr" lines
#   calls.log  — every invocation, for assertions
STATE="${FAKE_NMCLI_STATE:-/tmp/nmtest/state}"
mkdir -p "$STATE"
echo "$*" >> "$STATE/calls.log"

if [ ! -f "$STATE/profiles" ]; then
    cat > "$STATE/profiles" <<'EOF'
Wired connection 1|aaaaaaaa-1111-1111-1111-111111111111|eth0|192.168.137.110/24|192.168.137.1|8.8.8.8|2001:db8::1/64
EOF
fi

show_profile() {   # name or uuid
    local rec
    rec=$(grep -E "^[^|]*\|[^|]*\|$1\|" "$STATE/profiles")
    if [ -z "$rec" ]; then
        rec=$(grep -E "^$1\|" "$STATE/profiles")
    fi
    if [ -z "$rec" ]; then
        rec=$(grep -E "^[^|]*\|$1(\||$)" "$STATE/profiles")
    fi
    [ -z "$rec" ] && return 1
    local name uuid ifname addr gw dns v6
    IFS='|' read -r name uuid ifname addr gw dns v6 <<< "$rec"
    cat <<EOF
connection.id:$name
connection.uuid:$uuid
connection.interface-name:$ifname
ipv4.method:manual
ipv4.addresses:$addr
ipv4.gateway:$gw
ipv4.dns:$dns
ipv6.addresses:$v6
EOF
}

if [ "$1" = "-t" ]; then
    if [ "$2" = "-f" ]; then        # -t -f NAME,UUID connection show
        while IFS='|' read -r name uuid _; do
            echo "$name:$uuid"
        done < "$STATE/profiles"
    elif [ "$2" = "connection" ]; then   # -t connection show <name>
        show_profile "$4" || { echo "Error: connection not found" >&2; exit 1; }
    elif [ "$2" = "device" ]; then        # -t device show <nic>
        local rec ifname addr gw dns v6
        rec=$(grep -E "^[^|]*\|[^|]*\|$4\|" "$STATE/profiles")
        IFS='|' read -r _ _ ifname addr gw dns v6 <<< "$rec"
        echo "GENERAL.DEVICE:$4"
        echo "GENERAL.STATE:100 (connected)"
        if [ -n "$addr" ]; then
            echo "IP4.ADDRESS[1]:$addr"
            echo "IP4.GATEWAY:$gw"
            if [ -n "$dns" ]; then
                n=1
                IFS=',' read -ra dl <<< "$dns"
                for d in "${dl[@]}"; do echo "IP4.DNS[$n]:$d"; n=$((n+1)); done
            fi
            echo "IP6.ADDRESS[1]:fe80::1234:5678:9abc:def0/64"
            [ -n "$v6" ] && echo "IP6.ADDRESS[2]:$v6"
        fi
    fi
    exit 0
fi

case "$1 $2" in
"connection add")
    # connection add type ethernet ifname ethX con-name ethX
    nic=$6
    uuid="bbbbbbbb-$nic-1111-1111-111111111111"
    [ "$nic" = "eth0" ] && uuid="cccccccc-1111-1111-1111-111111111111"
    grep -q "|$nic|" "$STATE/profiles" && { echo "Error: connection already exists" >&2; exit 1; }
    echo "$nic|$uuid|$nic||||" >> "$STATE/profiles"
    echo "Connection '$nic' ($uuid) successfully added."
    ;;
"connection modify")
    # connection modify <uuid> ipv4.method manual ipv4.addresses CIDR
    #   ipv4.gateway GW ipv4.dns DNS ipv6.method manual|disabled|ignore [ipv6.addresses V6]
    # like real nmcli, only the given properties change; others keep
    # their current values
    uuid=$3
    local rec
    rec=$(grep -E "^[^|]*\|$uuid(\||$)" "$STATE/profiles")
    IFS='|' read -r name _ ifname addr gw dns v6 <<< "$rec"
    shift 3
    while [ -n "$1" ]; do
        case "$1" in
        ipv4.addresses) addr=$2 ;;
        ipv4.gateway)   gw=$2 ;;
        ipv4.dns)       dns=$2 ;;
        ipv6.addresses) v6=$2 ;;
        ipv6.method)    [ "$2" = "disabled" -o "$2" = "ignore" ] && v6= ;;
        esac
        shift 2
    done
    grep -v "^$name|" "$STATE/profiles" > "$STATE/profiles.tmp"
    echo "$name|$uuid|$ifname|$addr|$gw|$dns|$v6" >> "$STATE/profiles.tmp"
    mv "$STATE/profiles.tmp" "$STATE/profiles"
    ;;
"connection up")
    exit 0
    ;;
"connection delete")
    uuid=$3
    local rec name
    rec=$(grep -E "^[^|]*\|$uuid(\||$)" "$STATE/profiles")
    IFS='|' read -r name _ <<< "$rec"
    grep -v "^$name|" "$STATE/profiles" > "$STATE/profiles.tmp"
    mv "$STATE/profiles.tmp" "$STATE/profiles"
    ;;
*)
    echo "fake-nmcli: unhandled: $*" >&2
    exit 1
    ;;
esac
exit 0

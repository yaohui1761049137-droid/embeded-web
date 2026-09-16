#!/bin/sh
# chrony_acl_apply.sh — apply one web-managed NTP access rule to chrony
# and persist it.  Root-only; the web CGI reaches it through the
# www-data sudoers whitelist (every argument is re-validated here too).
#
#   chrony_acl_apply.sh add allow|deny <CIDR>   # runtime (chronyc) + file
#   chrony_acl_apply.sh remove <CIDR>           # file rewrite + chronyd restart
#
# chrony 3.4 facts this is built on (verified against the 3.4 sources):
#  - `chronyc allow|deny` changes the ACL at run time, effective
#    immediately, but is NOT persistent (lost on any chronyd restart);
#  - there is no runtime "remove rule" or "list rules" command;
#  - SIGHUP is a quit signal in 3.4 (NOT a config reload) — never send it.
#  Rule removal therefore rewrites the file and restarts chronyd — the only
#  deterministic way to shrink the runtime ACL (PPS re-locks within ~1 min).
ACL=/etc/chrony/acl-web.conf
HEADER='# Web-managed NTP access rules (chrony allow/deny) - do not edit by hand.'

die() { echo "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "错误: 需要 root 权限"

valid_cidr() {
    case "$1" in
    *:*)
        printf '%s' "$1" | grep -qE '^[0-9a-fA-F:]+(/[0-9]{1,3})?$' || return 1
        ;;
    *)
        printf '%s' "$1" | grep -qE '^[0-9]{1,3}(\.[0-9]{1,3}){3}(/[0-9]{1,2})?$' || return 1
        ;;
    esac
    pfx=${1#*/}
    [ "$pfx" = "$1" ] && return 0            # no /prefix
    case "$1" in
    *:*) [ "$pfx" -le 128 ] || return 1 ;;
    *)   [ "$pfx" -le 32 ] || return 1 ;;
    esac
    return 0
}

op=$1; shift
[ -f "$ACL" ] || { printf '%s\n' "$HEADER" > "$ACL" && chmod 644 "$ACL"; }

case "$op" in
add)
    action=$1; cidr=$2
    [ "$action" = allow ] || [ "$action" = deny ] || die "错误: 动作必须是 allow 或 deny"
    valid_cidr "$cidr" || die "错误: 无效的 CIDR: $cidr"
    grep -qxF "$action $cidr" "$ACL" && die "错误: 规则已存在: $action $cidr"
    chronyc "$action" "$cidr" >/dev/null 2>&1 || die "错误: chronyc $action 失败（CIDR 无效？）"
    { cat "$ACL"; echo "$action $cidr"; } > "$ACL.tmp" || die "错误: 写 $ACL.tmp 失败"
    chmod 644 "$ACL.tmp" && mv "$ACL.tmp" "$ACL" || die "错误: 更新 $ACL 失败"
    echo "OK: 已添加 $action $cidr（即时生效，已持久化）"
    ;;
remove)
    cidr=$1
    valid_cidr "$cidr" || die "错误: 无效的 CIDR: $cidr"
    if grep -qxF "allow $cidr" "$ACL"; then
        line="allow $cidr"
    elif grep -qxF "deny $cidr" "$ACL"; then
        line="deny $cidr"
    else
        die "错误: 规则不存在: $cidr"
    fi
    grep -vxF "$line" "$ACL" > "$ACL.tmp" || die "错误: 写 $ACL.tmp 失败"
    chmod 644 "$ACL.tmp" && mv "$ACL.tmp" "$ACL" || die "错误: 更新 $ACL 失败"
    systemctl restart chrony || die "错误: chrony 重启失败"
    systemctl is-active --quiet chrony || die "错误: chrony 重启后未运行"
    echo "OK: 已删除 $line（chrony 已重启重载，约 1 分钟重新锁定）"
    ;;
*)
    die "用法: $0 add allow|deny <CIDR> | $0 remove <CIDR>"
    ;;
esac

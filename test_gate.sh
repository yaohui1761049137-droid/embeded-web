#!/bin/bash
# test_gate.sh — 宿主机门卫单测（无需板子、无需 Lighttpd）
#
# 以受控环境变量直接运行 CGI 二进制（CGI 本来就是环境变量驱动的），
# 验证 gate 模块的 session → role → CSRF 阶梯。依赖 gate 的 DB_PATH
# 环境变量覆盖特性（默认 /var/db/myapp.db）。
#
# 网络配置测试通过 NMCLI_OVERRIDE 指向 fake_nmcli.sh 离线跑
# （无板子、无 NetworkManager、无 sudo）。
#
# Usage: ./test_gate.sh [src_dir]
# 依赖：宿主机 gcc（sqlite3.c 需 -lpthread -ldl），python3（策略组到期模拟 + DB 校验用）

set -e
SRC="${1:-$(cd "$(dirname "$0")" && pwd)/src}"
WORK=$(mktemp -d)
BIN=$WORK/bin
DB=$WORK/test.db
PASS=0
FAIL=0

trap 'rm -rf "$WORK"' EXIT

green() { echo -e "\033[32m$1\033[0m"; }
red()   { echo -e "\033[31m$1\033[0m"; }

assert_contains() {
    local desc="$1" pattern="$2" actual="$3"
    if echo "$actual" | grep -q "$pattern"; then
        green "  ✅ $desc"
        PASS=$((PASS + 1))
    else
        red "  ❌ $desc (expected to find: $pattern)"
        echo "$actual" | head -5 | sed 's/^/     /'
        FAIL=$((FAIL + 1))
    fi
}

assert_not_contains() {
    local desc="$1" pattern="$2" actual="$3"
    if echo "$actual" | grep -q "$pattern"; then
        red "  ❌ $desc (unexpected match: $pattern)"
        echo "$actual" | head -5 | sed 's/^/     /'
        FAIL=$((FAIL + 1))
    else
        green "  ✅ $desc"
        PASS=$((PASS + 1))
    fi
}

# ── Build host binaries ────────────────────────────────────────────
mkdir -p "$BIN"
echo "Compiling (host)…"
gcc -Wall -O2 -c -DSQLITE_THREADSAFE=0 "$SRC/sqlite3.c" -o "$WORK/sqlite3.o"
for src in login.cgi.c logout.cgi.c main.cgi.c network.cgi.c \
           user_list.cgi.c user_create.cgi.c user_passwd.cgi.c \
           user_toggle.cgi.c user_delete.cgi.c user_change_pass.cgi.c \
           timesync.cgi.c ntpmon.cgi.c log.cgi.c; do
    name=$(echo "$src" | sed 's/\.cgi\.c//').cgi
    gcc -Wall -O2 -o "$BIN/$name" "$SRC/$src" "$SRC/common.c" "$SRC/auth.c" \
        "$SRC/gate.c" "$SRC/users.c" "$SRC/nmcli.c" "$SRC/timesync.c" \
        "$SRC/ntpmon.c" "$SRC/sha256.c" \
        "$WORK/sqlite3.o" -lpthread -ldl
done
gcc -Wall -O2 -o "$BIN/db_init" "$SRC/db_init.c" "$SRC/auth.c" "$SRC/gate.c" \
    "$SRC/common.c" "$SRC/sha256.c" "$WORK/sqlite3.o" -lpthread -ldl
gcc -Wall -O2 -o "$BIN/test_users" "$(dirname "$0")/test_users.c" \
    "$SRC/users.c" "$SRC/auth.c" "$SRC/sha256.c" "$WORK/sqlite3.o" -lpthread -ldl
echo ""

# ── Seed temp DB (root / testpass123) ──────────────────────────────
# ADR-0002: first db_init creates root with password_changed_at=0 (forced
# change on first login). Second run hits the UPDATE branch → writes now,
# unlocking root so Test 4 keeps asserting "Location: /cgi-bin/main.cgi".
DB_PATH="$DB" "$BIN/db_init" testpass123 > /dev/null
DB_PATH="$DB" "$BIN/db_init" testpass123 > /dev/null

# ── Test 1: 无 cookie → JSON 错误（不碰数据库）──────────────────────
echo "1. Gate: no cookie → JSON error"
RESP=$(DB_PATH="$DB" QUERY_STRING="action=get" "$BIN/network.cgi")
assert_contains "Not authenticated" "Not authenticated" "$RESP"
assert_contains "error status" '"status":"error"' "$RESP"

# ── Test 2: 无 cookie → 页面模式 302 ────────────────────────────────
echo "2. Gate: page mode no cookie → redirect"
RESP=$(DB_PATH="$DB" "$BIN/main.cgi")
assert_contains "302 redirect" "Status: 302" "$RESP"
assert_contains "to index.html" "Location: /index.html" "$RESP"

# ── Test 3: 无效 session → JSON 错误 ────────────────────────────────
echo "3. Gate: invalid session cookie"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="session_id=deadbeef" \
       QUERY_STRING="action=get" "$BIN/network.cgi")
assert_contains "Not authenticated" "Not authenticated" "$RESP"

# ── Test 4: 登录成功 → 302 + 双 Cookie ──────────────────────────────
echo "4. Login: valid credentials"
BODY="user=root&pass=testpass123"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} REMOTE_ADDR=127.0.0.1 "$BIN/login.cgi")
assert_contains "302 redirect" "Status: 302" "$RESP"
assert_contains "session cookie" "session_id=" "$RESP"
assert_contains "csrf cookie" "csrf_token=" "$RESP"
assert_contains "HttpOnly on session" "HttpOnly" "$RESP"
assert_contains "SameSite=Lax" "SameSite=Lax" "$RESP"
assert_contains "to main.cgi" "Location: /cgi-bin/main.cgi" "$RESP"
SID=$(echo "$RESP" | grep -o "session_id=[^;]*" | head -1)
CSRF=$(echo "$RESP" | grep -o "csrf_token=[^;]*" | head -1)
CSRF_VAL=$(echo "$CSRF" | sed 's/csrf_token=//')

# ── Test 5: 登录失败 ────────────────────────────────────────────────
echo "5. Login: wrong password"
BODY="user=root&pass=wrongpass"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/login.cgi")
assert_contains "error status" '"status":"error"' "$RESP"

# ── Test 6: main.cgi 有效 session → 通过门卫 ────────────────────────
echo "6. Page gate: valid session"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" "$BIN/main.cgi")
assert_contains "session passed" "window._currentUser" "$RESP"
assert_contains "root role injected" 'role:"root"' "$RESP"

# ── Test 7-7f: network.cgi against fake nmcli (NMCLI_OVERRIDE) ──────
# Offline: no board, no NetworkManager, no sudo.  ROLLBACK_FILE keeps
# the eth0 rollback state inside the scratch dir instead of /var/db.
export NMCLI_OVERRIDE="$(dirname "$0")/fake_nmcli.sh"
export NMCLI_NO_SUDO=1
export ROLLBACK_FILE="$WORK/rollback.json"
export FAKE_NMCLI_STATE="$WORK/fake_state"

echo "7. JSON gate: valid session (reaches nmcli layer) — GET eth0"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=get&port=eth0" "$BIN/network.cgi")
assert_contains "gate passed (nmcli data, not auth)" '"status":"ok"' "$RESP"
assert_contains "fake ipv4 parsed" '"ip":"192.168.137.110"' "$RESP"
assert_contains "fake mask parsed" '"mask":"255.255.255.0"' "$RESP"
assert_contains "fake dns parsed" '"dns":"8.8.8.8"' "$RESP"
assert_contains "fake ipv6 parsed" '"ipv6":"2001:db8::1/64"' "$RESP"

echo "7b. network.cgi SET eth0 (valid CSRF) → applies + arms rollback"
BODY="ip=192.168.8.99&mask=255.255.255.0&gateway=192.168.8.1&dns=8.8.8.8,114.114.114.114&ipv6=2001:db8::2/64&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=set&port=eth0" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/network.cgi")
assert_contains "configure ok" '"status":"ok"' "$RESP"
assert_contains "save message" "保存成功" "$RESP"
RB=$(cat "$WORK/rollback.json" 2>/dev/null)
assert_contains "rollback armed (confirmed=0)" "confirmed=0" "$RB"
assert_contains "rollback old cidr" "old_cidr=192.168.137.110/24" "$RB"
assert_contains "rollback new cidr" "new_cidr=192.168.8.99/24" "$RB"

echo "7c. GET eth0 reflects applied config"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=get&port=eth0" "$BIN/network.cgi")
assert_contains "new ip" '"ip":"192.168.8.99"' "$RESP"
assert_contains "new dns (list joined)" '"dns":"8.8.8.8,114.114.114.114"' "$RESP"
assert_contains "new ipv6" '"ipv6":"2001:db8::2/64"' "$RESP"

echo "7d. Unknown NIC rejected"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=get&port=zz" "$BIN/network.cgi")
assert_contains "invalid nic" "无效的网口" "$RESP"

echo "7e. eth1: no profile → auto-created on set, then readable"
BODY="ip=10.0.0.5&mask=255.255.255.0&gateway=10.0.0.1&dns=&ipv6=&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=set&port=eth1" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/network.cgi")
assert_contains "eth1 set ok" '"status":"ok"' "$RESP"
CALLS=$(cat "$FAKE_NMCLI_STATE/calls.log")
assert_contains "profile auto-create invoked" "connection add type ethernet ifname eth1 con-name eth1" "$CALLS"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=get&port=eth1" "$BIN/network.cgi")
assert_contains "eth1 ip" '"ip":"10.0.0.5"' "$RESP"

echo "7f. Gateway migration: setting eth0 gateway clears eth2's"
echo 'Wired connection 2|aaaaaaaa-2222-2222-2222-222222222222|eth2|192.168.8.20/24|192.168.8.1|10.10.10.10|' >> "$FAKE_NMCLI_STATE/profiles"
BODY="ip=192.168.8.99&mask=255.255.255.0&gateway=192.168.9.1&dns=8.8.8.8,114.114.114.114&ipv6=2001:db8::2/64&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=set&port=eth0" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/network.cgi")
assert_contains "migration set ok" '"status":"ok"' "$RESP"
ETH2=$(grep '^Wired connection 2|' "$FAKE_NMCLI_STATE/profiles")
if echo "$ETH2" | grep -q '192.168.8.20/24||10.10.10.10'; then
    green "  ✅ eth2 gateway cleared, ip/dns preserved"
    PASS=$((PASS + 1))
else
    red "  ❌ eth2 gateway not cleared: $ETH2"
    FAIL=$((FAIL + 1))
fi

# ── Test 8: root 角色 → user_list 放行 ──────────────────────────────
echo "8. Role ladder: root allowed"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" "$BIN/user_list.cgi")
assert_contains "user list ok" '"status":"ok"' "$RESP"
assert_contains "root in list" '"username":"root"' "$RESP"

# ── Test 9: CSRF 缺失 → 拒绝 ────────────────────────────────────────
echo "9. CSRF ladder: missing token"
BODY="username=testadmin&password=TestPass123!"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/user_create.cgi")
assert_contains "CSRF rejected" "CSRF token invalid" "$RESP"

# ── Test 9b: CSRF 错误 → 拒绝 ───────────────────────────────────────
echo "9b. CSRF ladder: wrong token"
BODY="username=testadmin&password=TestPass123!&csrf_token=deadbeef"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/user_create.cgi")
assert_contains "CSRF rejected" "CSRF token invalid" "$RESP"

# ── Test 10: CSRF 有效 → 创建用户（校验入库）────────────────────────
echo "10. CSRF ladder: valid token → user created"
BODY="username=testadmin&password=TestPass123!&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/user_create.cgi")
assert_contains "user created" '"status":"ok"' "$RESP"
if command -v python3 > /dev/null; then
    N=$(python3 -c "import sqlite3;c=sqlite3.connect('$DB');\
print(c.execute(\"SELECT COUNT(*) FROM users WHERE username='testadmin'\").fetchone()[0])" 2>/dev/null || echo 0)
    if [ "$N" = "1" ]; then
        green "  ✅ user present in DB"
        PASS=$((PASS + 1))
    else
        red "  ❌ user not found in DB (count=$N)"
        FAIL=$((FAIL + 1))
    fi
fi

# ── Test 11: admin 角色 → user_list 拒绝 ────────────────────────────
echo "11. Role ladder: admin forbidden"
BODY="user=testadmin&pass=TestPass123!"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} REMOTE_ADDR=127.0.0.1 "$BIN/login.cgi")
ADMIN_SID=$(echo "$RESP" | grep -o "session_id=[^;]*" | head -1)
ADMIN_CSRF=$(echo "$RESP" | grep -o "csrf_token=[^;]*" | head -1)
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$ADMIN_SID; $ADMIN_CSRF" "$BIN/user_list.cgi")
assert_contains "Forbidden" "Forbidden" "$RESP"

# ── Test 11b: root 禁用 testadmin → 会话立即失效 ─────────────────────
echo "11b. Toggle: root disables testadmin"
ADMIN_ID=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" "$BIN/user_list.cgi" | \
    sed -n 's/.*"id":\([0-9]*\),"username":"testadmin".*/\1/p')
BODY="user_id=$ADMIN_ID&enabled=0&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/user_toggle.cgi")
assert_contains "toggle ok" '"status":"ok"' "$RESP"

# ── Test 11c: 被禁用户已有会话立即失效 ────────────────────────────────
echo "11c. Disabled user's session dies immediately"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$ADMIN_SID; $ADMIN_CSRF" \
       QUERY_STRING="action=get" "$BIN/network.cgi")
assert_contains "rejected" "Not authenticated" "$RESP"

# ── Test 11d: 恢复启用 ───────────────────────────────────────────────
echo "11d. Toggle: root re-enables testadmin"
BODY="user_id=$ADMIN_ID&enabled=1&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/user_toggle.cgi")
assert_contains "toggle ok" '"status":"ok"' "$RESP"

# ── Test 11e: root 删除 testadmin → 列表不再出现 ─────────────────────
echo "11e. Delete: root removes testadmin"
BODY="user_id=$ADMIN_ID&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/user_delete.cgi")
assert_contains "deleted" '"status":"ok"' "$RESP"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" "$BIN/user_list.cgi")
if echo "$RESP" | grep -q "testadmin"; then
    red "  ❌ 11e: testadmin still in user list"
    FAIL=$((FAIL + 1))
else
    green "  ✅ 11e: testadmin gone from user list"
    PASS=$((PASS + 1))
fi

# ── Test 13: login GET 有效 session → 跳 main ───────────────────────
echo "13. Login GET: valid session → redirect main"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" REQUEST_METHOD=GET "$BIN/login.cgi")
assert_contains "redirect main" "Location: /cgi-bin/main.cgi" "$RESP"

# ── Test 14: login GET 无 session → 跳 index ────────────────────────
echo "14. Login GET: no session → redirect index"
RESP=$(DB_PATH="$DB" REQUEST_METHOD=GET "$BIN/login.cgi")
assert_contains "redirect index" "Location: /index.html" "$RESP"

# ── Test 15: logout → 清双 Cookie ───────────────────────────────────
echo "15. Logout: clears cookies"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" "$BIN/logout.cgi")
assert_contains "302 redirect" "Status: 302" "$RESP"
assert_contains "session cleared" "session_id=;" "$RESP"
assert_contains "csrf cleared" "csrf_token=;" "$RESP"

# ── Test 16: logout 后 session 失效 ─────────────────────────────────
echo "16. Session invalid after logout"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID" "$BIN/main.cgi")
assert_contains "redirect to index" "Location: /index.html" "$RESP"

# ── ADR-0002 密码策略组（Test 17-26）─────────────────────────────────
echo "17. Login root again (unlocked — goes to main, not change.html)"
BODY="user=root&pass=testpass123"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} REMOTE_ADDR=127.0.0.1 "$BIN/login.cgi")
assert_contains "302 redirect" "Status: 302" "$RESP"
assert_contains "to main.cgi" "Location: /cgi-bin/main.cgi" "$RESP"
SID=$(echo "$RESP" | grep -o "session_id=[^;]*" | head -1)
CSRF=$(echo "$RESP" | grep -o "csrf_token=[^;]*" | head -1)
CSRF_VAL=$(echo "$CSRF" | sed 's/csrf_token=//')

echo "18. Create user with policy-valid password"
BODY="username=testadmin2&password=TestPass123!&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/user_create.cgi")
assert_contains "user created" '"status":"ok"' "$RESP"

echo "19. Weak passwords refused by policy"
BODY="username=weak1&password=Abc123!&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/user_create.cgi")
assert_contains "short password refused" "密码长度需为" "$RESP"
BODY="username=weak2&password=abcdefghij&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/user_create.cgi")
assert_contains "letters-only refused" "密码需包含大写字母" "$RESP"

echo "20. Non-ASCII password refused"
BODY="username=weak3&password=Abcd1234!中&csrf_token=$CSRF_VAL"
BLEN=$(printf '%s' "$BODY" | wc -c)   # UTF-8: bytes ≠ chars
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       REQUEST_METHOD=POST CONTENT_LENGTH=$BLEN "$BIN/user_create.cgi")
assert_contains "non-ASCII refused" "密码只能包含 ASCII 可见字符" "$RESP"

echo "21. Login testadmin2 → admin session (cookie-consistent)"
BODY="user=testadmin2&pass=TestPass123!"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} REMOTE_ADDR=127.0.0.1 "$BIN/login.cgi")
assert_contains "to main.cgi" "Location: /cgi-bin/main.cgi" "$RESP"
ADMIN_SID=$(echo "$RESP" | grep -o "session_id=[^;]*" | head -1)
ADMIN_CSRF=$(echo "$RESP" | grep -o "csrf_token=[^;]*" | head -1)
ADMIN_CSRF_VAL=$(echo "$ADMIN_CSRF" | sed 's/csrf_token=//')

echo "22. Change password: wrong current password"
BODY="old_password=WrongPass1!&new_password=NewPass123!&csrf_token=$ADMIN_CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$ADMIN_SID; $ADMIN_CSRF" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/user_change_pass.cgi")
assert_contains "current password rejected" "当前密码不正确" "$RESP"

echo "23. Change password: reuse of current password refused"
BODY="old_password=TestPass123!&new_password=TestPass123!&csrf_token=$ADMIN_CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$ADMIN_SID; $ADMIN_CSRF" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/user_change_pass.cgi")
assert_contains "reuse refused" "新密码不能与当前密码相同" "$RESP"

echo "24. Self change: second session kicked, current survives"
BODY="user=testadmin2&pass=TestPass123!"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/login.cgi")
SID2B=$(echo "$RESP" | grep -o "session_id=[^;]*" | head -1)
CSRF2B=$(echo "$RESP" | grep -o "csrf_token=[^;]*" | head -1)
BODY="old_password=TestPass123!&new_password=NewPass456!&csrf_token=$ADMIN_CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$ADMIN_SID; $ADMIN_CSRF" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/user_change_pass.cgi")
assert_contains "change ok" '"status":"ok"' "$RESP"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID2B; $CSRF2B" \
       QUERY_STRING="action=get" "$BIN/network.cgi")
assert_contains "kicked session dead" "Not authenticated" "$RESP"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$ADMIN_SID; $ADMIN_CSRF" \
       QUERY_STRING="action=get" "$BIN/network.cgi")
assert_contains "current session alive (gate passed)" "无效的网口" "$RESP"

echo "25. Login with new password"
BODY="user=testadmin2&pass=NewPass456!"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/login.cgi")
assert_contains "302 redirect" "Status: 302" "$RESP"
assert_contains "to main.cgi" "Location: /cgi-bin/main.cgi" "$RESP"

echo "26. Expiry: forced change flow (gate blocks, change CGI exempt)"
python3 - "$DB" <<'PY'
import sqlite3, sys
db = sys.argv[1]
c = sqlite3.connect(db)
c.execute("PRAGMA wal_checkpoint(TRUNCATE)")
c.execute("UPDATE users SET password_changed_at=0 WHERE username='testadmin2'")
c.commit()
PY
BODY="user=testadmin2&pass=NewPass456!"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/login.cgi")
assert_contains "302 redirect" "Status: 302" "$RESP"
assert_contains "to change.html" "Location: /change.html" "$RESP"
EXP_SID=$(echo "$RESP" | grep -o "session_id=[^;]*" | head -1)
EXP_CSRF=$(echo "$RESP" | grep -o "csrf_token=[^;]*" | head -1)
EXP_CSRF_VAL=$(echo "$EXP_CSRF" | sed 's/csrf_token=//')
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$EXP_SID; $EXP_CSRF" \
       QUERY_STRING="action=get" "$BIN/network.cgi")
assert_contains "gate blocks expired user" "密码已过期,请先修改密码" "$RESP"
BODY="old_password=NewPass456!&new_password=Recovered1!&csrf_token=$EXP_CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$EXP_SID; $EXP_CSRF" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/user_change_pass.cgi")
assert_contains "change ok" '"status":"ok"' "$RESP"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$EXP_SID; $EXP_CSRF" \
       QUERY_STRING="action=get" "$BIN/network.cgi")
assert_contains "recovered, gate passes again" "无效的网口" "$RESP"

# ── Test 27-27f: timesync.cgi — status & receiver mode (offline) ────
# Offline: no board, no chronyd, no serial port.  CHRONYC_OVERRIDE
# swaps chronyc for fake_chronyc.sh; TIMESYNC_STATUS_DIR / TIMESYNC_DEV
# point at scratch files instead of /run/pps_tod and /dev/ttyS7.
export CHRONYC_OVERRIDE="$(dirname "$0")/fake_chronyc.sh"
export TIMESYNC_STATUS_DIR="$WORK/ts_state"
export TIMESYNC_DEV="$WORK/ts_dev"
export TIMESYNC_MODE_FILE="$WORK/ut986_mode"
mkdir -p "$TIMESYNC_STATUS_DIR"
cat > "$TIMESYNC_STATUS_DIR/status" <<'EOF'
ts=2026-09-14 11:00:00
good=1
last_good_age_s=0
offset_us=+23
anchor_fresh=1
sec_gated=0
sec_noedge=0
sec_noanchor=0
sec_badtod=0
rt_vs_mono_us=0
EOF
cat > "$TIMESYNC_STATUS_DIR/watchdog.state" <<'EOF'
ts=2026-09-14 11:00:01
mode=OK
bad_since=0
last_action=0
observe_until=0
strikes=
EOF
: > "$TIMESYNC_DEV"

echo "27. timesync status: chrony + state files parsed"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=status" "$BIN/timesync.cgi")
assert_contains "status ok" '"status":"ok"' "$RESP"
assert_contains "source PPS parsed" '"name":"PPS"' "$RESP"
assert_contains "flags+state merged (chrony 3.x layout)" '"ms":"#\*"' "$RESP"
assert_contains "reach parsed" '"reach":"377"' "$RESP"
assert_contains "last sample hex-decoded (chrony 4.x row)" "46us" "$RESP"
assert_contains "tracking system time" '"system_time":"0.000001438"' "$RESP"
assert_contains "tracking ref name (14-col layout)" '"ref_name":"PPS"' "$RESP"
assert_contains "tracking leap status" '"leap_status":"Normal"' "$RESP"
assert_contains "pps_tod good" '"good":"1"' "$RESP"
assert_contains "pps_tod offset" '"offset_us":"+23"' "$RESP"
assert_contains "watchdog mode" '"mode":"OK"' "$RESP"
assert_contains "freshness emitted" '"age_s"' "$RESP"
assert_contains "no last-set mode yet" '"last_mode":""' "$RESP"

echo "27b. timesync status: missing state files degrade gracefully"
rm -f "$TIMESYNC_STATUS_DIR/status" "$TIMESYNC_STATUS_DIR/watchdog.state"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=status" "$BIN/timesync.cgi")
assert_contains "still ok" '"status":"ok"' "$RESP"
assert_contains "pps_tod degraded" '"pps_tod":{"ok":false,"age_s":-1}' "$RESP"
assert_contains "watchdog degraded" '"watchdog":{"ok":false,"age_s":-1}' "$RESP"

echo "27c. setmode missing CSRF → rejected"
BODY="mode=gps"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=setmode" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/timesync.cgi")
assert_contains "csrf rejected" "CSRF token invalid" "$RESP"

echo "27d. setmode invalid mode → rejected (whitelist)"
BODY="mode=wifi&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=setmode" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/timesync.cgi")
assert_contains "invalid mode" "无效的接收机模式" "$RESP"

echo "27e. setmode valid mode → exact payload bytes on device"
BODY="mode=bds&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=setmode" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/timesync.cgi")
assert_contains "command sent" '"status":"ok"' "$RESP"
assert_contains "mode echoed" '"mode":"bds"' "$RESP"
printf '$CFGGNSS,h70*08\r\n$CFGSAVE,h10*06\r\n' > "$WORK/ts_expect"
if cmp -s "$WORK/ts_expect" "$TIMESYNC_DEV"; then
    green "  ✅ payload + checksums byte-exact (bds)"
    PASS=$((PASS + 1))
else
    red "  ❌ payload mismatch: $(od -c "$TIMESYNC_DEV" | head -3)"
    FAIL=$((FAIL + 1))
fi
MF=$(cat "$TIMESYNC_MODE_FILE" 2>/dev/null)
assert_contains "mode file saved (bds)" "mode=bds" "$MF"

echo "27f. setmode gnss (factory default) payload"
BODY="mode=gnss&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=setmode" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/timesync.cgi")
assert_contains "command sent" '"status":"ok"' "$RESP"
printf '$CFGGNSS,h70717D*7D\r\n$CFGSAVE,h10*06\r\n' > "$WORK/ts_expect2"
if cmp -s "$WORK/ts_expect2" "$TIMESYNC_DEV"; then
    green "  ✅ payload + checksums byte-exact (gnss)"
    PASS=$((PASS + 1))
else
    red "  ❌ payload mismatch: $(od -c "$TIMESYNC_DEV" | head -3)"
    FAIL=$((FAIL + 1))
fi
MF=$(cat "$TIMESYNC_MODE_FILE" 2>/dev/null)
assert_contains "mode file saved (gnss)" "mode=gnss" "$MF"

echo "27g. status reflects last-set receiver mode"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=status" "$BIN/timesync.cgi")
assert_contains "last_mode now gnss" '"last_mode":"gnss"' "$RESP"
assert_contains "set_at recorded" '"set_at":"20' "$RESP"

# ── Test 28-28k: ntpmon.cgi — NTP ACL + traffic stats (offline) ─────
# Offline: fake ACL helper (NTPMON_HELPER_OVERRIDE) + scratch acl/csv.
export NTPMON_ACL_FILE="$WORK/ntpmon_acl.conf"
export NTPMON_CSV="$WORK/ntpmon_stats.csv"
export NTPMON_HELPER_OVERRIDE="$(dirname "$0")/fake_ntp_acl_helper.sh"
export FAKE_ACL_STATE="$WORK/fake_acl"
mkdir -p "$FAKE_ACL_STATE"
cat > "$NTPMON_ACL_FILE" <<'EOF'
# Web-managed NTP access rules (chrony allow/deny) - do not edit by hand.
allow 192.168.137.0/24
EOF
T0=$(( $(date +%s) - 180 ))
cat > "$NTPMON_CSV" <<EOF
$T0,10,0,5,0,0,7,0,0,0
$((T0+60)),22,0,5,0,0,9,0,0,0
$((T0+120)),1,0,5,0,0,1,0,0,0
$((T0+180)),13,0,5,0,0,3,5,0,0
EOF

echo "28. ntpmon stats: acl rules + series (counter reset) + bars"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=stats" "$BIN/ntpmon.cgi")
assert_contains "status ok" '"status":"ok"' "$RESP"
assert_contains "acl rule listed" '"action":"allow","cidr":"192.168.137.0/24"' "$RESP"
assert_contains "series t array" '"series":{"t":\[' "$RESP"
assert_contains "per-minute w/ reset break" '"m":\[-1,12,-1,12\]' "$RESP"
assert_contains "cumulative raw values" '"c":\[10,22,1,13\]' "$RESP"
assert_contains "eth totals" '"total":\[3,5,0,0\]' "$RESP"
assert_contains "eth 1m delta (latest interval)" '"last_1m":\[2,5,0,0\]' "$RESP"
assert_contains "eth 1h delta (reset clamped)" '"last_1h":\[0,5,0,0\]' "$RESP"
assert_contains "eth 5h delta present" '"last_5h":\[0,5,0,0\]' "$RESP"
assert_contains "eth 24h delta present" '"last_24h":\[0,5,0,0\]' "$RESP"
assert_contains "csv flagged ok" '"csv_ok":true' "$RESP"

echo "28b. ntpmon stats: missing CSV degrades gracefully"
mv "$NTPMON_CSV" "$NTPMON_CSV.hidden"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=stats" "$BIN/ntpmon.cgi")
assert_contains "csv flagged missing" '"csv_ok":false' "$RESP"
assert_contains "empty series" '"t":\[\]' "$RESP"
mv "$NTPMON_CSV.hidden" "$NTPMON_CSV"

echo "28c. ntpmon stats: admin session can read"
BODY="user=testadmin2&pass=Recovered1!"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/login.cgi")
ADM_SID=$(echo "$RESP" | grep -o "session_id=[^;]*" | head -1)
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$ADM_SID" \
       QUERY_STRING="action=stats" "$BIN/ntpmon.cgi")
assert_contains "stats allowed for admin" '"status":"ok"' "$RESP"

echo "28d. acl_op: admin forbidden (root-only)"
BODY="op=add&action=allow&cidr=10.20.30.0/24&csrf_token=x"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$ADM_SID" \
       QUERY_STRING="action=acl_op" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/ntpmon.cgi")
assert_contains "Forbidden" "Forbidden" "$RESP"

echo "28e. acl_op: missing CSRF rejected"
BODY="op=add&action=allow&cidr=10.20.30.0/24"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=acl_op" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/ntpmon.cgi")
assert_contains "CSRF rejected" "CSRF token invalid" "$RESP"

echo "28f. acl_op: invalid CIDR rejected"
BODY="op=add&action=allow&cidr=999.1.2.3/33&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=acl_op" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/ntpmon.cgi")
assert_contains "invalid CIDR" "无效的 CIDR" "$RESP"

echo "28g. acl_op: root add → helper invoked with exact args"
BODY="op=add&action=allow&cidr=10.20.30.0/24&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=acl_op" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/ntpmon.cgi")
assert_contains "add ok" '"status":"ok"' "$RESP"
CALLS=$(cat "$FAKE_ACL_STATE/calls.log")
assert_contains "helper got add args" "add allow 10.20.30.0/24" "$CALLS"

echo "28h. acl_op: same CIDR with the OTHER action is allowed (allow+deny coexist)"
# chrony applies deny at equal prefix length, so flipping a subnet from allow
# to deny must not require deleting the allow first (which restarts chronyd).
BODY="op=add&action=deny&cidr=192.168.137.0/24&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=acl_op" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/ntpmon.cgi")
assert_contains "different action accepted" '"status":"ok"' "$RESP"
CALLS=$(cat "$FAKE_ACL_STATE/calls.log")
assert_contains "helper got add deny" "add deny 192.168.137.0/24" "$CALLS"

echo "28h2. acl_op: exact duplicate (same action + CIDR) still rejected"
BODY="op=add&action=allow&cidr=192.168.137.0/24&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=acl_op" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/ntpmon.cgi")
assert_contains "duplicate rejected" "规则已存在" "$RESP"

echo "28i. acl_op: root remove → helper gets the resolved action"
BODY="op=remove&cidr=192.168.137.0/24&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=acl_op" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/ntpmon.cgi")
assert_contains "remove ok" '"status":"ok"' "$RESP"
CALLS=$(cat "$FAKE_ACL_STATE/calls.log")
assert_contains "helper got remove + action" "remove allow 192.168.137.0/24" "$CALLS"

echo "28i2. acl_op: remove's action is part of the identity (deny not present)"
# Only `allow 192.168.137.0/24` exists, so asking to remove the DENY for the
# same prefix must not silently remove the allow.
BODY="op=remove&action=deny&cidr=192.168.137.0/24&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=acl_op" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/ntpmon.cgi")
assert_contains "wrong action not silently matched" "规则不存在" "$RESP"
CALLS=$(cat "$FAKE_ACL_STATE/calls.log")
assert_not_contains "helper NOT called for the wrong action" "remove deny 192.168.137.0/24" "$CALLS"

echo "28i3. acl_op: remove with a bogus action rejected"
BODY="op=remove&action=maybe&cidr=192.168.137.0/24&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=acl_op" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/ntpmon.cgi")
assert_contains "bad remove action rejected" "动作必须是 allow 或 deny" "$RESP"

echo "28j. acl_op: removing a nonexistent rule rejected"
BODY="op=remove&cidr=172.16.0.0/12&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=acl_op" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/ntpmon.cgi")
assert_contains "not exists" "规则不存在" "$RESP"

echo "28k. acl_op: helper failure surfaced to the UI"
BODY="op=add&action=allow&cidr=10.99.0.0/16&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" FAKE_ACL_FAIL=1 \
       QUERY_STRING="action=acl_op" REQUEST_METHOD=POST \
       CONTENT_LENGTH=${#BODY} "$BIN/ntpmon.cgi")
assert_contains "helper error surfaced" "模拟失败" "$RESP"

# ── Test 29-29h: log.cgi — system log viewer (offline) ──────────────
# LOGVIEW_ROOT points the id→path whitelist at a scratch tree so the CGI
# runs without touching /var/log.
LOGVIEW_ROOT="$WORK/logroot"
export LOGVIEW_ROOT
mkdir -p "$LOGVIEW_ROOT/pps_tod" "$LOGVIEW_ROOT/lighttpd"
TODAY=$(date +%Y-%m-%d)
for i in $(seq 1 300); do echo "line-$i"; done > "$LOGVIEW_ROOT/pps_tod/pps_tod_$TODAY.log"
echo "EVENT_MARKER bootstrap" >> "$LOGVIEW_ROOT/pps_tod/pps_tod_$TODAY.log"
printf 'wd-a\nwd-b\n' > "$LOGVIEW_ROOT/pps_tod/watchdog.log"
: > "$LOGVIEW_ROOT/lighttpd/error.log"
# access.log intentionally absent → the "missing file" branch
# a file bigger than the 256 KB tail window, marker only at the start
python3 - "$LOGVIEW_ROOT/lighttpd/access.log" <<'PY'
import sys
with open(sys.argv[1], "w") as f:
    f.write("HEAD_MARKER_ONLY_AT_TOP\n")
    for i in range(20000):
        f.write("padding line %05d aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n" % i)
    f.write("TAIL_MARKER_AT_BOTTOM\n")
PY

echo "29. log.cgi sources: whitelist metadata + dated filename"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=sources" "$BIN/log.cgi")
assert_contains "status ok" '"status":"ok"' "$RESP"
assert_contains "pps_tod listed" '"id":"pps_tod"' "$RESP"
assert_contains "dated filename built" "pps_tod_$TODAY.log" "$RESP"
assert_contains "existing file has size" '"exists":true' "$RESP"
assert_contains "file size reported" '"size":1120046' "$RESP"

echo "29b. log.cgi tail: newest N lines, truncation flagged"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=tail&id=pps_tod&lines=3" "$BIN/log.cgi")
assert_contains "marker line returned" 'EVENT_MARKER bootstrap' "$RESP"
assert_contains "third-from-last returned" 'line-299' "$RESP"
assert_not_contains "fourth-from-last dropped" 'line-298' "$RESP"
assert_contains "returned=3" '"returned":3' "$RESP"
assert_contains "truncated flagged" '"truncated":true' "$RESP"
assert_contains "matched counted" '"matched":301' "$RESP"

echo "29c. log.cgi tail: grep filters before the line cap"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=tail&id=pps_tod&lines=10&grep=EVENT_MARKER" "$BIN/log.cgi")
assert_contains "filtered line returned" 'EVENT_MARKER bootstrap' "$RESP"
assert_contains "matched=1" '"matched":1' "$RESP"
assert_contains "returned=1" '"returned":1' "$RESP"
assert_not_contains "non-matching lines dropped" 'line-300' "$RESP"

echo "29d. log.cgi tail: lines clamped to >=1"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=tail&id=pps_tod&lines=0" "$BIN/log.cgi")
assert_contains "clamped to one line" '"returned":1' "$RESP"

echo "29e. log.cgi tail: only the tail window is read"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=tail&id=lighttpd_acc&lines=5" "$BIN/log.cgi")
assert_contains "bottom marker reached" 'TAIL_MARKER_AT_BOTTOM' "$RESP"
assert_contains "window flag set" '"windowed":true' "$RESP"
assert_contains "file size reported" '"exists":true' "$RESP"
assert_not_contains "top of a >256KB file not read" 'HEAD_MARKER_ONLY_AT_TOP' "$RESP"

echo "29f. log.cgi tail: missing file degrades gracefully"
rm -f "$LOGVIEW_ROOT/lighttpd/access.log"
RESP_MISSING=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=tail&id=lighttpd_acc&lines=5" "$BIN/log.cgi")
assert_contains "missing file: still ok" '"status":"ok"' "$RESP_MISSING"
assert_contains "missing file: exists=false" '"exists":false' "$RESP_MISSING"
assert_contains "missing file: empty list" '"lines":\[\]' "$RESP_MISSING"

echo "29g. log.cgi: unknown id / path traversal rejected"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=tail&id=../../etc/passwd" "$BIN/log.cgi")
assert_contains "traversal rejected" "未知的日志源" "$RESP"
assert_not_contains "no file content leaked" 'root:' "$RESP"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=tail&id=nginx" "$BIN/log.cgi")
assert_contains "unknown id rejected" "未知的日志源" "$RESP"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=bogus" "$BIN/log.cgi")
assert_contains "bad action rejected" "无效的 action" "$RESP"

echo "29h. log.cgi: admin forbidden (root-only)"
BODY="user=testadmin2&pass=Recovered1!"
RESP_A=$(printf '%s' "$BODY" | DB_PATH="$DB" REQUEST_METHOD=POST \
         CONTENT_LENGTH=${#BODY} REMOTE_ADDR=127.0.0.1 "$BIN/login.cgi")
A_SID=$(echo "$RESP_A" | grep -o "session_id=[^;]*" | head -1)
A_CSRF=$(echo "$RESP_A" | grep -o "csrf_token=[^;]*" | head -1)
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$A_SID; $A_CSRF" \
       QUERY_STRING="action=sources" "$BIN/log.cgi")
assert_contains "Forbidden for admin" "Forbidden" "$RESP"
assert_not_contains "no source list leaked to admin" '"sources"' "$RESP"

# ── Test 30-30d: ntpmon nic block — per-interface serving detail ────
# The daemon's snapshot is embedded verbatim, so the whole stats response
# must still be valid JSON — that is the assertion that matters most.
NIC_FILE="$WORK/ntp_nic.json"
NOW=$(date +%s)
cat > "$NIC_FILE" <<EOF
{"ts":$NOW,"sec_t0":$((NOW-119)),"nics":[{"name":"eth0","ifindex":2,"req":50,"valid":48,"rsp":47,
"req_1m":5,"valid_1m":5,"rsp_1m":5,
"req_1h":20,"valid_1h":19,"rsp_1h":18,
"req_5h":45,"valid_5h":44,"rsp_5h":43,"req_24h":50,"valid_24h":48,"rsp_24h":47,
"windows_s":[60,3600,18000,86400],"buckets":[0,0,50],
"sec":[0,0,1,2,3]},
{"name":"eth1","ifindex":3,"req":70,"valid":70,"rsp":70,
"req_1m":7,"valid_1m":7,"rsp_1m":7,
"req_1h":70,"valid_1h":70,"rsp_1h":70,
"req_5h":70,"valid_5h":70,"rsp_5h":70,"req_24h":70,"valid_24h":70,"rsp_24h":70,
"windows_s":[60,3600,18000,86400],"buckets":[0,0,70],
"sec":[1,1,1,1,1]}],
"clients":{"eth0":[{"ip":"192.168.137.11","req":50,"valid":48,"last_age_s":3}],
"eth1":[{"ip":"192.168.1.121","req":70,"valid":70,"last_age_s":5}]},
"clients_total":2,"rsp_source":"iptables"}
EOF

echo "30. ntpmon stats: nic block embedded verbatim"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" NTPMON_NIC_FILE="$NIC_FILE" \
       QUERY_STRING="action=stats" "$BIN/ntpmon.cgi" | grep -v '^Content-Type' | grep -v '^$')
assert_contains "nic ok" '"nic":{"ok":true' "$RESP"
assert_contains "nic age computed" '"age_s":0' "$RESP"
assert_contains "eth0 client ip" '192.168.137.11' "$RESP"
assert_contains "eth1 client ip" '192.168.1.121' "$RESP"
assert_contains "response count passthrough" '"rsp":47' "$RESP"
assert_contains "1m window sums passthrough" '"req_1m":5' "$RESP"
assert_contains "1h window sums passthrough" '"req_1h":20' "$RESP"
assert_contains "window list passthrough" '"windows_s":\[60,3600,18000,86400\]' "$RESP"
assert_contains "5h sums passthrough" '"req_5h":45' "$RESP"
assert_contains "24h sums passthrough" '"req_24h":50' "$RESP"
assert_contains "per-second ring passthrough" '"sec":\[0,0,1,2,3\]' "$RESP"
assert_contains "per-second t0 passthrough" '"sec_t0":' "$RESP"
assert_contains "existing keys untouched" '"status":"ok","acl"' "$RESP"

echo "30b. ntpmon stats: whole response is still valid JSON"
if echo "$RESP" | python3 -c "
import json,sys
d=json.load(sys.stdin)
assert d['status']=='ok'
assert d['nic']['ok'] is True
assert d['nic']['nics'][1]['req_1h']==70
assert d['nic']['nics'][0]['req_1m']==5
assert d['nic']['nics'][0]['windows_s']==[60,3600,18000,86400]
assert d['nic']['nics'][0]['req_24h']==50
assert d['nic']['nics'][0]['sec']==[0,0,1,2,3]
assert d['nic']['nics'][1]['sec']==[1,1,1,1,1]
assert d['nic']['sec_t0']==d['nic']['ts']-119
assert d['nic']['clients']['eth0'][0]['ip']=='192.168.137.11'
assert d['eth']['names']==['eth0','eth1','eth2','eth3']
print('parsed ok')
" > /dev/null 2>&1; then
    assert_contains "json parses" "ok" "ok"
else
    assert_contains "json parses" "parse-failed" "$(echo "$RESP" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>&1 | head -2)"
fi

echo "30c. ntpmon stats: missing nic snapshot degrades"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" NTPMON_NIC_FILE="$WORK/nope.json" \
       QUERY_STRING="action=stats" "$BIN/ntpmon.cgi")
assert_contains "nic flagged missing" '"nic":{"ok":false}' "$RESP"
assert_contains "stats still ok" '"status":"ok","acl"' "$RESP"

echo "30d. ntpmon stats: oversized snapshot refused, not truncated"
python3 - "$WORK/nic_big.json" <<'PY'
import sys
with open(sys.argv[1], "w") as f:
    f.write('{"ts":1,"pad":"')
    f.write("x" * 200000)
    f.write('"}')
PY
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" NTPMON_NIC_FILE="$WORK/nic_big.json" \
       QUERY_STRING="action=stats" "$BIN/ntpmon.cgi")
assert_contains "too_large flagged" '"too_large":true' "$RESP"
if echo "$RESP" | grep -v '^Content-Type' | grep -v '^$' | \
   python3 -c "import json,sys; json.load(sys.stdin)" 2>/dev/null; then
    assert_contains "still valid json" "ok" "ok"
else
    assert_contains "still valid json" "parse-failed" "truncated object leaked"
fi

# ── Users module unit tests (direct API, offline) ──────────────────
echo ""
echo "── Users module unit tests ──"
set +e
"$BIN/test_users" "$WORK/users.db"
TU_RC=$?
set -e
if [ $TU_RC -ne 0 ]; then
    red "  ❌ users module tests failed (rc=$TU_RC)"
    FAIL=$((FAIL + 1))
else
    green "  ✅ users module tests passed"
    PASS=$((PASS + 1))
fi

# ── Frontend registry consistency (offline) ─────────────────────────
echo ""
echo "── Frontend registry consistency ──"
set +e
python3 "$(dirname "$0")/test_frontend.py" "$(dirname "$0")"
FE_RC=$?
set -e
if [ $FE_RC -ne 0 ]; then
    red "  ❌ frontend registry tests failed (rc=$FE_RC)"
    FAIL=$((FAIL + 1))
else
    green "  ✅ frontend registry consistent"
    PASS=$((PASS + 1))
fi

# ── Summary ─────────────────────────────────────────────────────────
echo ""
echo "========================================="
echo " Results: $PASS passed, $FAIL failed"
echo "========================================="
[ $FAIL -gt 0 ] && red "SOME TESTS FAILED" && exit 1
green "ALL TESTS PASSED"

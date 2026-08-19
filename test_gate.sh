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

# ── Build host binaries ────────────────────────────────────────────
mkdir -p "$BIN"
echo "Compiling (host)…"
gcc -Wall -O2 -c -DSQLITE_THREADSAFE=0 "$SRC/sqlite3.c" -o "$WORK/sqlite3.o"
for src in login.cgi.c logout.cgi.c main.cgi.c network.cgi.c \
           user_list.cgi.c user_create.cgi.c user_passwd.cgi.c \
           user_toggle.cgi.c user_delete.cgi.c user_change_pass.cgi.c; do
    name=$(echo "$src" | sed 's/\.cgi\.c//').cgi
    gcc -Wall -O2 -o "$BIN/$name" "$SRC/$src" "$SRC/common.c" "$SRC/auth.c" \
        "$SRC/gate.c" "$SRC/users.c" "$SRC/nmcli.c" "$SRC/sha256.c" \
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

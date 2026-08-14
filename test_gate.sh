#!/bin/bash
# test_gate.sh — 宿主机门卫单测（无需板子、无需 Lighttpd）
#
# 以受控环境变量直接运行 CGI 二进制（CGI 本来就是环境变量驱动的），
# 验证 gate 模块的 session → role → CSRF 阶梯。依赖 gate 的 DB_PATH
# 环境变量覆盖特性（默认 /var/db/myapp.db）。
#
# Usage: ./test_gate.sh [src_dir]
# 依赖：宿主机 gcc（sqlite3.c 需 -lpthread -ldl），python3（可选，仅 DB 校验用）

set -e
SRC="${1:-$(cd "$(dirname "$0")" && pwd)/src}"
WORK=$(mktemp -d)
BIN=$WORK/bin
DB=$WORK/test.db
PASS=0
FAIL=0

FAKE_PID=""
# Cleanup must also kill the pty fake board: an orphaned fake keeps the
# suite's stderr pipe open, which hangs any wrapper (tail) forever.
trap 'if [ -n "$FAKE_PID" ]; then kill "$FAKE_PID" 2>/dev/null; wait "$FAKE_PID" 2>/dev/null || true; fi; rm -rf "$WORK"' EXIT

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
for src in login.cgi.c logout.cgi.c main.cgi.c network.cgi.c action.cgi.c \
           user_list.cgi.c user_create.cgi.c user_passwd.cgi.c \
           user_toggle.cgi.c user_delete.cgi.c; do
    name=$(echo "$src" | sed 's/\.cgi\.c//').cgi
    gcc -Wall -O2 -o "$BIN/$name" "$SRC/$src" "$SRC/common.c" "$SRC/auth.c" \
        "$SRC/gate.c" "$SRC/users.c" "$SRC/remote.c" "$SRC/sha256.c" \
        "$WORK/sqlite3.o" -lpthread -ldl
done
gcc -Wall -O2 -o "$BIN/db_init" "$SRC/db_init.c" "$SRC/auth.c" "$SRC/gate.c" \
    "$SRC/common.c" "$SRC/sha256.c" "$WORK/sqlite3.o" -lpthread -ldl
gcc -Wall -O2 -o "$BIN/test_users" "$(dirname "$0")/test_users.c" \
    "$SRC/users.c" "$SRC/auth.c" "$SRC/sha256.c" "$WORK/sqlite3.o" -lpthread -ldl
gcc -Wall -O2 -o "$BIN/test_remote" "$(dirname "$0")/test_remote.c" \
    "$SRC/remote.c" "$SRC/common.c"
echo ""

# ── Seed temp DB (root / testpass123) ──────────────────────────────
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

# ── Test 7: network.cgi 有效 session → 门卫放行（走到串口层）─────────
echo "7. JSON gate: valid session (reaches serial layer)"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       QUERY_STRING="action=get" "$BIN/network.cgi")
assert_contains "gate passed (serial error, not auth)" "Cannot open serial port" "$RESP"

# ── Test 7b-7d: network.cgi against pty fake board (offline serial) ──
FAKE_OUT=$WORK/fake_slave
python3 "$(dirname "$0")/test_fake_board.py" ok "$WORK/fake.log" > "$FAKE_OUT" &
FAKE_PID=$!
while [ ! -s "$FAKE_OUT" ]; do sleep 0.05; done
FAKE_DEV=$(head -1 "$FAKE_OUT")

echo "7b. network.cgi GET against fake board (pty)"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       REMOTE_SERIAL_DEVICE_OVERRIDE="$FAKE_DEV" \
       QUERY_STRING="action=get" "$BIN/network.cgi")
assert_contains "status ok" '"status":"ok"' "$RESP"
assert_contains "fake ipv4 parsed end-to-end" '"ip":"10.0.0.1"' "$RESP"

echo "7c. network.cgi SET against fake board (pty, valid CSRF)"
BODY="ip=192.168.8.99&mask=255.255.255.0&gateway=192.168.8.1&ipv6=&csrf_token=$CSRF_VAL"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       REMOTE_SERIAL_DEVICE_OVERRIDE="$FAKE_DEV" \
       QUERY_STRING="action=set" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/network.cgi")
assert_contains "configure ok" '"status":"ok"' "$RESP"
assert_contains "save message" "保存成功" "$RESP"

echo "7d. Unknown board port rejected"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       REMOTE_SERIAL_DEVICE_OVERRIDE="$FAKE_DEV" \
       QUERY_STRING="action=get&port=zz" "$BIN/network.cgi")
assert_contains "invalid board" "Invalid board" "$RESP"

echo "7e. Board 2 (port=s4) via fake board"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       REMOTE_SERIAL_DEVICE_OVERRIDE="$FAKE_DEV" \
       QUERY_STRING="action=get&port=s4" "$BIN/network.cgi")
assert_contains "board 2 query ok" '"status":"ok"' "$RESP"
assert_contains "board 2 ipv4" '"ip":"10.0.0.1"' "$RESP"
kill "$FAKE_PID" 2>/dev/null
wait "$FAKE_PID" 2>/dev/null || true
FAKE_PID=""

# ── Test 8: root 角色 → user_list 放行 ──────────────────────────────
echo "8. Role ladder: root allowed"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" "$BIN/user_list.cgi")
assert_contains "user list ok" '"status":"ok"' "$RESP"
assert_contains "root in list" '"username":"root"' "$RESP"

# ── Test 9: CSRF 缺失 → 拒绝 ────────────────────────────────────────
echo "9. CSRF ladder: missing token"
BODY="username=testadmin&password=test123456"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/user_create.cgi")
assert_contains "CSRF rejected" "CSRF token invalid" "$RESP"

# ── Test 9b: CSRF 错误 → 拒绝 ───────────────────────────────────────
echo "9b. CSRF ladder: wrong token"
BODY="username=testadmin&password=test123456&csrf_token=deadbeef"
RESP=$(printf '%s' "$BODY" | DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" \
       REQUEST_METHOD=POST CONTENT_LENGTH=${#BODY} "$BIN/user_create.cgi")
assert_contains "CSRF rejected" "CSRF token invalid" "$RESP"

# ── Test 10: CSRF 有效 → 创建用户（校验入库）────────────────────────
echo "10. CSRF ladder: valid token → user created"
BODY="username=testadmin&password=test123456&csrf_token=$CSRF_VAL"
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
BODY="user=testadmin&pass=test123456"
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

# ── Test 12: action.cgi 有效 session → 门卫放行（此前零测试）────────
echo "12. action.cgi: valid session (reaches serial layer)"
RESP=$(DB_PATH="$DB" HTTP_COOKIE="$SID; $CSRF" "$BIN/action.cgi")
assert_contains "gate passed (serial error, not auth)" "Cannot open /dev/ttyFIQ0" "$RESP"

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

# ── Remote module unit tests (pty fake board, offline) ─────────────
echo ""
echo "── Remote module unit tests ──"
set +e
"$BIN/test_remote" "$(dirname "$0")/test_fake_board.py"
TR_RC=$?
set -e
if [ $TR_RC -ne 0 ]; then
    red "  ❌ remote module tests failed (rc=$TR_RC)"
    FAIL=$((FAIL + 1))
else
    green "  ✅ remote module tests passed"
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

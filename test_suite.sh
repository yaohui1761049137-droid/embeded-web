#!/bin/bash
# test_suite.sh — Phase 2 端到端测试套件
# Usage: ./test_suite.sh [board_ip] [root_password]
# Default IP: 192.168.137.100; 默认密码需满足 ADR-0002 策略(部署时设置的强密码)
# 注意:root 密码若处于"强制改密"状态(存量库首次部署后),先手动改一次密再跑本套件

set -e
IP="${1:-192.168.137.100}"
PW="${2:-Abcd1234!xyz}"
PASS=0
FAIL=0

green() { echo -e "\033[32m$1\033[0m"; }
red()   { echo -e "\033[31m$1\033[0m"; }

assert_status() {
    local desc="$1" expected="$2" actual="$3"
    if echo "$actual" | grep -q "\"status\":\"$expected\""; then
        green "  ✅ $desc"
        PASS=$((PASS + 1))
    else
        red "  ❌ $desc (expected status=$expected, got: $actual)"
        FAIL=$((FAIL + 1))
    fi
}

assert_contains() {
    local desc="$1" pattern="$2" actual="$3"
    if echo "$actual" | grep -q "$pattern"; then
        green "  ✅ $desc"
        PASS=$((PASS + 1))
    else
        red "  ❌ $desc (expected to find: $pattern)"
        FAIL=$((FAIL + 1))
    fi
}

echo "========================================="
echo " Phase 2 Test Suite — $IP"
echo "========================================="
echo ""

# ── Setup: clean up test users from previous runs ──────────────────
# Login as root first
SETUP_RESP=$(curl -sk -D - -X POST -d "user=root&pass=$PW" \
    https://$IP/cgi-bin/login.cgi 2>&1)
ROOT_SID=$(echo "$SETUP_RESP" | grep -o "session_id=[^;]*" | head -1)
ROOT_CSRF=$(echo "$SETUP_RESP" | grep -o "csrf_token=[^;]*" | head -1)
ROOT_CSRF_VAL=$(echo "$ROOT_CSRF" | sed 's/csrf_token=//')

# Delete leftover test users (ignore errors)
curl -sk -b "$ROOT_SID; $ROOT_CSRF" \
    -d "user_id=2&csrf_token=$ROOT_CSRF_VAL" \
    "https://$IP/cgi-bin/user_delete.cgi" > /dev/null 2>&1 || true
curl -sk -b "$ROOT_SID; $ROOT_CSRF" \
    -d "user_id=3&csrf_token=$ROOT_CSRF_VAL" \
    "https://$IP/cgi-bin/user_delete.cgi" > /dev/null 2>&1 || true

TEST_USER="test_$(date +%s | tail -c 5)"
echo "Test user: $TEST_USER"
echo ""

# ── Test 1: HTTP → HTTPS redirect ───────────────────────────────────
echo "1. HTTP → HTTPS"
RESP=$(curl -sI http://$IP/ 2>&1)
assert_contains "301 redirect" "301 Moved Permanently" "$RESP"

# ── Test 2: HTTPS static file ───────────────────────────────────────
echo "2. HTTPS static file"
RESP=$(curl -sk https://$IP/ 2>&1)
assert_contains "index.html served" "<html" "$RESP"

# ── Test 3: HSTS header ─────────────────────────────────────────────
echo "3. HSTS header"
RESP=$(curl -sk -I https://$IP/ 2>&1)
assert_contains "HSTS present" "strict-transport-security" "$RESP"

# ── Test 4: Login success ───────────────────────────────────────────
echo "4. Login (valid credentials)"
RESP=$(curl -sk -D - -X POST -d "user=root&pass=$PW" \
    https://$IP/cgi-bin/login.cgi 2>&1)
assert_contains "302 redirect" "HTTP/1.1 302" "$RESP"
assert_contains "session_id cookie" "session_id=" "$RESP"
assert_contains "csrf_token cookie" "csrf_token=" "$RESP"
assert_contains "HttpOnly on session_id" "HttpOnly" "$RESP"
assert_contains "Secure on cookies" "Secure" "$RESP"
assert_contains "SameSite=Lax" "SameSite=Lax" "$RESP"

SID=$(echo "$RESP" | grep -o "session_id=[^;]*" | head -1)
CSRF_COOKIE=$(echo "$RESP" | grep -o "csrf_token=[^;]*" | head -1)
CSRF_VAL=$(echo "$CSRF_COOKIE" | sed 's/csrf_token=//')

# ── Test 5: Login failure ───────────────────────────────────────────
echo "5. Login (wrong password)"
RESP=$(curl -sk -X POST -d "user=root&pass=wrong" \
    https://$IP/cgi-bin/login.cgi 2>&1)
assert_status "Invalid credentials" "error" "$RESP"

# ── Test 6: Login (SQL injection attempt) ───────────────────────────
echo "6. Login (SQL injection)"
RESP=$(curl -sk -X POST -d "user=root' OR 1=1 --&pass=x" \
    https://$IP/cgi-bin/login.cgi 2>&1)
assert_status "SQL injection rejected" "error" "$RESP"

# ── Test 7: Protected CGI with valid session ────────────────────────
echo "7. Protected CGI (valid session)"
RESP=$(curl -sk -b "$SID" https://$IP/cgi-bin/main.cgi 2>&1)
assert_contains "control panel loaded" "Control Panel" "$RESP"

# ── Test 8: Protected CGI without session ───────────────────────────
echo "8. Protected CGI (no session)"
RESP=$(curl -sk https://$IP/cgi-bin/network.cgi?action=get 2>&1)
assert_status "Not authenticated" "error" "$RESP"

# ── Test 9: 本地模式通信 (ADR-0001: 无 port = 本机 FIFO + handler-local.sh) ──
echo "9. 本地模式通信 (network.cgi GET, no port)"
RESP=$(curl -sk -b "$SID" "https://$IP/cgi-bin/network.cgi?action=get" 2>&1)
assert_status "Local query OK" "ok" "$RESP"
assert_contains "IPv4 data returned" "ipv4" "$RESP"
assert_contains "IPv6 data returned" "ipv6" "$RESP"

# ── Test 10: CSRF protection (no token) ─────────────────────────────
echo "10. CSRF protection (no token)"
RESP=$(curl -sk -b "$SID" \
    -d "ip=192.168.8.100&mask=255.255.255.0&gateway=192.168.8.1" \
    "https://$IP/cgi-bin/network.cgi?action=set" 2>&1)
assert_status "CSRF rejected" "error" "$RESP"
assert_contains "CSRF message" "CSRF token invalid" "$RESP"

# ── Test 11: CSRF protection (wrong token) ──────────────────────────
echo "11. CSRF protection (wrong token)"
RESP=$(curl -sk -b "$SID; $CSRF_COOKIE" \
    -d "ip=192.168.8.100&mask=255.255.255.0&gateway=192.168.8.1&ipv6=&csrf_token=deadbeef" \
    "https://$IP/cgi-bin/network.cgi?action=set" 2>&1)
assert_status "Wrong CSRF rejected" "error" "$RESP"

# ── Test 12: CSRF success (valid token, Board 2 port=s4) ───────────────
echo "12. CSRF success (Board 2, port=s4)"
# B2 现为 192.168.8.199:同 IP 重写是 no-op,避免把远端板改来改去
RESP=$(curl -sk -b "$SID; $CSRF_COOKIE" \
    -d "ip=192.168.8.199&mask=255.255.255.0&gateway=192.168.8.1&ipv6=&csrf_token=$CSRF_VAL" \
    "https://$IP/cgi-bin/network.cgi?action=set&port=s4" 2>&1)
assert_status "Valid CSRF accepted" "ok" "$RESP"

# ── Test 12b: Audit log recorded after network SET ───────────────────
echo "12b. Audit log (network_set)"
AUDIT_LOG=$(sshpass -p 'root' ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 root@$IP \
    "python3 -c \"
import sqlite3
conn = sqlite3.connect('/var/db/myapp.db')
conn.execute('PRAGMA wal_checkpoint(FULL)')
cur = conn.cursor()
cur.execute(\\\"SELECT COUNT(*) FROM audit_log WHERE action='network_set'\\\")
print(cur.fetchone()[0])
conn.close()
\"" 2>/dev/null || echo "0")
if [ "$AUDIT_LOG" -gt 0 ]; then
    green "  ✅ Audit log recorded (count=$AUDIT_LOG)"
    PASS=$((PASS + 1))
else
    red "  ❌ Audit log not found (sshpass + python3 required on board)"
    FAIL=$((FAIL + 1))
fi

# ── Test 12c: Board 2 (port=s4) serial GET ────────────────────────────
echo "12c. Board 2 (ttyS4) serial GET"
RESP=$(curl -sk -b "$SID" "https://$IP/cgi-bin/network.cgi?action=get&port=s4" 2>&1)
assert_status "Board 2 query OK" "ok" "$RESP"
assert_contains "Board 2 IPv4 data" "ipv4" "$RESP"

# ── Test 12d: 本地模式 SET(相同 IP 重写, no-op,不断连) ──────────────
echo "12d. 本地模式 SET (同 IP 重写)"
LOCAL_GET=$(curl -sk -b "$SID" "https://$IP/cgi-bin/network.cgi?action=get" 2>&1)
LOCAL_IP=$(echo "$LOCAL_GET" | sed -n 's/.*"ip":"\([^"]*\)".*/\1/p')
LOCAL_MASK=$(echo "$LOCAL_GET" | sed -n 's/.*"mask":"\([^"]*\)".*/\1/p')
LOCAL_GW=$(echo "$LOCAL_GET" | sed -n 's/.*"gateway":"\([^"]*\)".*/\1/p')
if [ -n "$LOCAL_IP" ] && [ -n "$LOCAL_MASK" ]; then
    RESP=$(curl -sk -b "$SID; $CSRF_COOKIE" \
        -d "ip=$LOCAL_IP&mask=$LOCAL_MASK&gateway=$LOCAL_GW&ipv6=&csrf_token=$CSRF_VAL" \
        "https://$IP/cgi-bin/network.cgi?action=set" 2>&1)
    assert_status "本地 SET no-op 通过" "ok" "$RESP"
else
    red "  ❌ 无法解析本机当前 IP,跳过本地 SET 测试"
    FAIL=$((FAIL + 1))
fi

# ── Test 12e: 本地模式 confirm(取消看门狗回滚) ─────────────────────
echo "12e. 本地模式 confirm (watchdog cancel)"
RESP=$(curl -sk -b "$SID; $CSRF_COOKIE" \
    -d "csrf_token=$CSRF_VAL" \
    "https://$IP/cgi-bin/network.cgi?action=confirm" 2>&1)
assert_status "Confirm OK" "ok" "$RESP"

# ── Test 13: Root API ───────────────────────────────────────────────
echo "13. Root API (user_list)"
RESP=$(curl -sk -b "$SID" https://$IP/cgi-bin/user_list.cgi 2>&1)
assert_status "User list OK" "ok" "$RESP"
assert_contains "Root user in list" "root" "$RESP"

# ── Test 14: Create admin user ──────────────────────────────────────
echo "14. Create admin user"
TEST_PASS="Test1234!xyz"   # 满足 ADR-0002 策略(10+ 四类)
RESP=$(curl -sk -b "$SID; $CSRF_COOKIE" \
    -d "username=$TEST_USER&password=$TEST_PASS&csrf_token=$CSRF_VAL" \
    "https://$IP/cgi-bin/user_create.cgi" 2>&1)
assert_status "User created" "ok" "$RESP"

# ── Test 15: Root self-protection ───────────────────────────────────
echo "15. Root self-protection (delete self)"
RESP=$(curl -sk -b "$SID; $CSRF_COOKIE" \
    -d "user_id=1&csrf_token=$CSRF_VAL" \
    "https://$IP/cgi-bin/user_delete.cgi" 2>&1)
assert_status "Cannot delete self" "error" "$RESP"

# ── Test 16: Logout ─────────────────────────────────────────────────
echo "16. Logout"
RESP=$(curl -sk -b "$SID" https://$IP/cgi-bin/logout.cgi -D - 2>&1)
assert_contains "302 redirect" "HTTP/1.1 302" "$RESP"
assert_contains "session_id cleared" "session_id=;" "$RESP"
assert_contains "csrf_token cleared" "csrf_token=;" "$RESP"

# ── Test 17: Session invalid after logout ───────────────────────────
echo "17. Session invalid after logout"
RESP=$(curl -sk -b "$SID" -D - https://$IP/cgi-bin/main.cgi 2>&1)
# Should redirect to /index.html (302), not serve content
assert_contains "Redirect after logout" "ocation: /index.html" "$RESP"

# ── Test 18: Login as created admin user ────────────────────────────
echo "18. Login as admin user"
RESP=$(curl -sk -D - -X POST -d "user=$TEST_USER&pass=$TEST_PASS" \
    https://$IP/cgi-bin/login.cgi 2>&1)
assert_contains "Admin login OK" "session_id=" "$RESP"

ADMIN_SID=$(echo "$RESP" | grep -o "session_id=[^;]*" | head -1)
ADMIN_CSRF=$(echo "$RESP" | grep -o "csrf_token=[^;]*" | head -1)
ADMIN_CSRF_VAL=$(echo "$ADMIN_CSRF" | sed 's/csrf_token=//')

# ── Test 19: Admin cannot access root API ────────────────────────────
echo "19. Admin cannot access root API"
RESP=$(curl -sk -b "$ADMIN_SID" https://$IP/cgi-bin/user_list.cgi 2>&1)
assert_status "Admin forbidden" "error" "$RESP"

# ── Test 20: Admin can access normal CGI ────────────────────────────
echo "20. Admin can access normal CGI"
RESP=$(curl -sk -b "$ADMIN_SID" https://$IP/cgi-bin/main.cgi 2>&1)
assert_contains "Admin access OK" "Control Panel" "$RESP"

# ── ADR-0002: 密码策略用例 ──────────────────────────────────────────

# ── Test 21: Create user with weak password rejected ────────────────
echo "21. Create user 弱密码拒绝"
RESP=$(curl -sk -b "$SID; $CSRF_COOKIE" \
    -d "username=weakuser&password=abc123&csrf_token=$CSRF_VAL" \
    "https://$IP/cgi-bin/user_create.cgi" 2>&1)
assert_status "弱密码拒绝" "error" "$RESP"

# ── Test 22: Create user with Chinese chars rejected ────────────────
echo "22. Create user 含中文密码拒绝"
RESP=$(curl -sk -b "$SID; $CSRF_COOKIE" \
    -d "username=cnuser&password=Abcd1234!中&csrf_token=$CSRF_VAL" \
    "https://$IP/cgi-bin/user_create.cgi" 2>&1)
assert_status "中文密码拒绝" "error" "$RESP"

# ── Test 23: Self change with wrong old password rejected ───────────
echo "23. 自改密旧密码错误拒绝"
RESP=$(curl -sk -b "$ADMIN_SID; $CSRF_COOKIE" \
    -d "old_password=wrongpass&new_password=Qwer5678!aa&csrf_token=$ADMIN_CSRF_VAL" \
    "https://$IP/cgi-bin/user_change_pass.cgi" 2>&1)
assert_status "旧密码错误拒绝" "error" "$RESP"

# ── Test 24: Self change to same password rejected ──────────────────
echo "24. 自改密与旧密码相同拒绝"
RESP=$(curl -sk -b "$ADMIN_SID; $CSRF_COOKIE" \
    -d "old_password=$TEST_PASS&new_password=$TEST_PASS&csrf_token=$ADMIN_CSRF_VAL" \
    "https://$IP/cgi-bin/user_change_pass.cgi" 2>&1)
assert_status "复用旧密码拒绝" "error" "$RESP"

# ── Test 25: Self change ok + kicks other sessions ──────────────────
echo "25. 自改密成功 + 踢其他会话"
# 第二个管理员会话(旧密码登录)
ADMIN2_RESP=$(curl -sk -D - -X POST -d "user=$TEST_USER&pass=$TEST_PASS" \
    https://$IP/cgi-bin/login.cgi 2>&1)
ADMIN2_SID=$(echo "$ADMIN2_RESP" | grep -o "session_id=[^;]*" | head -1)
TEST_PASS2="Qwer5678!ab"
RESP=$(curl -sk -b "$ADMIN_SID; $CSRF_COOKIE" \
    -d "old_password=$TEST_PASS&new_password=$TEST_PASS2&csrf_token=$ADMIN_CSRF_VAL" \
    "https://$IP/cgi-bin/user_change_pass.cgi" 2>&1)
assert_status "改密成功" "ok" "$RESP"
# 旧会话(ADMIN_SID)被踢,另一会话(ADMIN2_SID)也被踢
RESP=$(curl -sk -b "$ADMIN2_SID" "https://$IP/cgi-bin/network.cgi?action=get" 2>&1)
assert_status "其他会话被踢" "error" "$RESP"
RESP=$(curl -sk -b "$ADMIN_SID" "https://$IP/cgi-bin/network.cgi?action=get" 2>&1)
assert_status "改密当前会话保留" "ok" "$RESP"

# ── Test 26: Login with new password works ──────────────────────────
echo "26. 新密码登录成功"
RESP=$(curl -sk -D - -X POST -d "user=$TEST_USER&pass=$TEST_PASS2" \
    https://$IP/cgi-bin/login.cgi 2>&1)
assert_contains "新密码登录 OK" "session_id=" "$RESP"
ADMIN_SID=$(echo "$RESP" | grep -o "session_id=[^;]*" | head -1)

# ── Test 27: 到期强制改密(password_changed_at=0 模拟存量/到期) ──────
echo "27. 到期强制改密流程"
# 用 python3 把测试用户置为"从未设置密码"(存量迁移状态)
sshpass -p 'root' ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 root@$IP \
    "python3 -c \"
import sqlite3
conn = sqlite3.connect('/var/db/myapp.db')
conn.execute('PRAGMA wal_checkpoint(FULL)')
conn.execute(\\\"UPDATE users SET password_changed_at=0 WHERE username='$TEST_USER'\\\")
conn.commit()
conn.close()
\"" 2>/dev/null || true
RESP=$(curl -sk -D - -X POST -d "user=$TEST_USER&pass=$TEST_PASS2" \
    https://$IP/cgi-bin/login.cgi 2>&1)
assert_contains "到期登录 → change.html" "ocation: /change.html" "$RESP"
ADMIN_SID=$(echo "$RESP" | grep -o "session_id=[^;]*" | head -1)
CSRF_COOKIE=$(echo "$RESP" | grep -o "csrf_token=[^;]*" | head -1)
CSRF_VAL=$(echo "$CSRF_COOKIE" | sed 's/csrf_token=//')
RESP=$(curl -sk -b "$ADMIN_SID" "https://$IP/cgi-bin/network.cgi?action=get" 2>&1)
assert_contains "到期期间拦截" "密码已过期" "$RESP"
RESP=$(curl -sk -b "$ADMIN_SID; $CSRF_COOKIE" \
    -d "old_password=$TEST_PASS2&new_password=$TEST_PASS&csrf_token=$CSRF_VAL" \
    "https://$IP/cgi-bin/user_change_pass.cgi" 2>&1)
assert_status "到期强制改密成功" "ok" "$RESP"
RESP=$(curl -sk -b "$ADMIN_SID" "https://$IP/cgi-bin/network.cgi?action=get" 2>&1)
assert_status "改密后功能恢复" "ok" "$RESP"

# ── Summary ──────────────────────────────────────────────────────────
echo ""
echo "========================================="
echo " Results: $PASS passed, $FAIL failed"
echo "========================================="

if [ $FAIL -gt 0 ]; then
    red "SOME TESTS FAILED"
    exit 1
else
    green "ALL TESTS PASSED"
    exit 0
fi

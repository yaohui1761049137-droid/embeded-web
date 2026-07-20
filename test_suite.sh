#!/bin/bash
# test_suite.sh — Phase 2 端到端测试套件
# Usage: ./test_suite.sh [board_ip]
# Default: 192.168.137.100

set -e
IP="${1:-192.168.137.100}"
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
SETUP_RESP=$(curl -sk -D - -X POST -d "user=root&pass=admin" \
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
RESP=$(curl -sk -D - -X POST -d "user=root&pass=admin" \
    https://$IP/cgi-bin/login.cgi 2>&1)
assert_contains "302 redirect" "HTTP/2 302" "$RESP"
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

# ── Test 9: Serial communication ────────────────────────────────────
echo "9. Serial communication (network.cgi GET)"
RESP=$(curl -sk -b "$SID" "https://$IP/cgi-bin/network.cgi?action=get" 2>&1)
assert_status "Serial query OK" "ok" "$RESP"
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

# ── Test 12: CSRF success (valid token) ─────────────────────────────
echo "12. CSRF success (valid token)"
RESP=$(curl -sk -b "$SID; $CSRF_COOKIE" \
    -d "ip=192.168.8.203&mask=255.255.255.0&gateway=192.168.8.1&ipv6=&csrf_token=$CSRF_VAL" \
    "https://$IP/cgi-bin/network.cgi?action=set" 2>&1)
assert_status "Valid CSRF accepted" "ok" "$RESP"

# ── Test 13: Root API ───────────────────────────────────────────────
echo "13. Root API (user_list)"
RESP=$(curl -sk -b "$SID" https://$IP/cgi-bin/user_list.cgi 2>&1)
assert_status "User list OK" "ok" "$RESP"
assert_contains "Root user in list" "root" "$RESP"

# ── Test 14: Create admin user ──────────────────────────────────────
echo "14. Create admin user"
RESP=$(curl -sk -b "$SID; $CSRF_COOKIE" \
    -d "username=$TEST_USER&password=test123456&csrf_token=$CSRF_VAL" \
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
assert_contains "302 redirect" "HTTP/2 302" "$RESP"
assert_contains "session_id cleared" "session_id=;" "$RESP"
assert_contains "csrf_token cleared" "csrf_token=;" "$RESP"

# ── Test 17: Session invalid after logout ───────────────────────────
echo "17. Session invalid after logout"
RESP=$(curl -sk -b "$SID" -D - https://$IP/cgi-bin/main.cgi 2>&1)
# Should redirect to /index.html (302), not serve content
assert_contains "Redirect after logout" "ocation: /index.html" "$RESP"

# ── Test 18: Login as created admin user ────────────────────────────
echo "18. Login as admin user"
RESP=$(curl -sk -D - -X POST -d "user=$TEST_USER&pass=test123456" \
    https://$IP/cgi-bin/login.cgi 2>&1)
assert_contains "Admin login OK" "session_id=" "$RESP"

ADMIN_SID=$(echo "$RESP" | grep -o "session_id=[^;]*" | head -1)

# ── Test 19: Admin cannot access root API ────────────────────────────
echo "19. Admin cannot access root API"
RESP=$(curl -sk -b "$ADMIN_SID" https://$IP/cgi-bin/user_list.cgi 2>&1)
assert_status "Admin forbidden" "error" "$RESP"

# ── Test 20: Admin can access normal CGI ────────────────────────────
echo "20. Admin can access normal CGI"
RESP=$(curl -sk -b "$ADMIN_SID" https://$IP/cgi-bin/main.cgi 2>&1)
assert_contains "Admin access OK" "Control Panel" "$RESP"

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

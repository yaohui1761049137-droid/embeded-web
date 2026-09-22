"""Check timesync.cgi?action=status for the new GGA keys (with session)."""
import http.cookiejar
import json
import ssl
import sys
import urllib.parse
import urllib.request

BASE = "https://192.168.1.111"
USER = "root"
PASS = sys.argv[1] if len(sys.argv) > 1 else "Testpassword1234@@"

ctx = ssl._create_unverified_context()
cj = http.cookiejar.CookieJar()


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


op = urllib.request.build_opener(
    urllib.request.HTTPCookieProcessor(cj),
    urllib.request.HTTPSHandler(context=ctx),
    NoRedirect,
)

req = urllib.request.Request(
    BASE + "/cgi-bin/login.cgi",
    data=urllib.parse.urlencode({"user": USER, "pass": PASS}).encode(),
    method="POST")
try:
    r = op.open(req, timeout=10)
    print("login:", r.status)
except urllib.error.HTTPError as e:
    print("login:", e.code, "Location:", dict(e.headers).get("Location", ""))
print("cookies:", sorted(c.name for c in cj))
body = op.open(BASE + "/cgi-bin/timesync.cgi?action=status", timeout=10).read().decode(errors="replace")
d = json.loads(body)
p = d.get("pps_tod", {})
print("fix=%s sats=%s sats_age_s=%s good=%s" % (
    p.get("fix"), p.get("sats"), p.get("sats_age_s"), p.get("good")))
print("watchdog mode=%s  ts=%s" % (d.get("watchdog", {}).get("mode"), d.get("ts")))

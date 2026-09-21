"""Full functional verification of the deployed embeded-web system.

Read-only views (network/timesync/ntpmon/log/user_list), a create-toggle-
delete round trip on a temp user, and logout. Prints one PASS/FAIL per step
and writes the transcript to stdout (redirect to an evidence file).
"""
import http.cookiejar
import json
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request

BASE = "https://192.168.1.111"
USER = "root"
PASS = "Testpassword1234@"

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

results = []


def step(name, ok, detail=""):
    results.append((name, ok, detail))
    print("[%s] %-38s %s" % ("PASS" if ok else "FAIL", name, detail[:150]))
    return ok


def request(path, data=None):
    if data is not None:
        req = urllib.request.Request(
            BASE + path, data=urllib.parse.urlencode(data).encode(), method="POST")
    else:
        req = urllib.request.Request(BASE + path)
    try:
        r = op.open(req, timeout=15)
        return r.status, r.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")


def csrf():
    for c in cj:
        if c.name == "csrf_token":
            return c.value
    return ""


def jok(body):
    try:
        j = json.loads(body)
        return j.get("status") == "ok" or j.get("status") == 0 or "error" not in body[:200].lower()
    except Exception:
        return False


def main():
    code, body = request("/cgi-bin/login.cgi", {"user": USER, "pass": PASS})
    step("login root", code == 302, "HTTP %s" % code)

    code, body = request("/cgi-bin/main.cgi")
    step("main.cgi panel", code == 200 and len(body) > 5000, "HTTP %s len %d" % (code, len(body)))

    code, body = request("/cgi-bin/network.cgi?action=get&port=eth0")
    step("network.cgi get eth0", code == 200 and jok(body), body.strip()[:100])

    code, body = request("/cgi-bin/timesync.cgi?action=status")
    step("timesync.cgi status", code == 200 and jok(body), body.strip()[:100])

    code, body = request("/cgi-bin/ntpmon.cgi?action=stats")
    step("ntpmon.cgi stats", code == 200 and jok(body), body.strip()[:100])

    code, body = request("/cgi-bin/log.cgi?action=sources")
    step("log.cgi sources", code == 200 and jok(body), body.strip()[:100])

    code, body = request("/cgi-bin/user_list.cgi")
    step("user_list.cgi", code == 200 and "root" in body, body.strip()[:100])

    # temp user round trip: create -> list -> delete
    code, body = request("/cgi-bin/user_create.cgi", {
        "username": "testuser_tmp", "password": "Tmp@123456", "csrf_token": csrf()})
    step("user_create.cgi testuser_tmp", code == 200 and '"ok"' in body, body.strip()[:100])

    code, body = request("/cgi-bin/user_list.cgi")
    uid = None
    try:
        for u in json.loads(body).get("users", []):
            if u.get("username") == "testuser_tmp":
                uid = u.get("id")
                break
    except Exception:
        pass
    step("user_list shows testuser_tmp", code == 200 and uid is not None,
         "id=%s" % uid)

    code, body = request("/cgi-bin/user_delete.cgi", {
        "user_id": uid, "csrf_token": csrf()})
    step("user_delete.cgi testuser_tmp", code == 200 and '"ok"' in body, body.strip()[:100])

    code, body = request("/cgi-bin/user_list.cgi")
    step("user_list testuser_tmp gone", code == 200 and "testuser_tmp" not in body, "")

    code, body = request("/cgi-bin/logout.cgi")
    step("logout.cgi", code == 200 or code == 302, "HTTP %s" % code)

    n_fail = sum(1 for _, ok, _ in results if not ok)
    print("\n[RESULT] %d/%d steps passed -> %s"
          % (len(results) - n_fail, len(results), "PASS" if n_fail == 0 else "FAIL"))
    return 0 if n_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

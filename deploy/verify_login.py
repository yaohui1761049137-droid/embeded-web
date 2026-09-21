"""End-to-end login verification for the deployed embeded-web system.

Flow: admin first-login (forced change) -> set real password -> re-login ->
open control panel. Reproducible evidence for the headless deployment test.
"""
import http.cookiejar
import json
import ssl
import sys
import urllib.parse
import urllib.request

BASE = "https://192.168.1.111"
INITIAL_PW = "admin"
REAL_PW = "Testpassword1234@"

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


def post(path, fields):
    req = urllib.request.Request(
        BASE + path,
        data=urllib.parse.urlencode(fields).encode(),
        method="POST",
    )
    try:
        r = op.open(req, timeout=10)
        return r.status, r.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")


def get(path):
    req = urllib.request.Request(BASE + path)
    try:
        r = op.open(req, timeout=10)
        return r.status, r.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")


def csrf():
    for c in cj:
        if c.name == "csrf_token":
            return c.value
    return ""


def main():
    code, body = post("/cgi-bin/login.cgi", {"user": "root", "pass": INITIAL_PW})
    print("[1] initial login admin ->", code)
    print("    cookies:", sorted(c.name for c in cj))
    if code == 302:
        code, body = get("/change.html")
        if code == 200 and "change-card" in body:
            print("    -> /change.html reached (forced change) OK")
        else:
            print("    -> /change.html UNEXPECTED:", code, body[:200])
            return 1
    else:
        print("    -> UNEXPECTED body head:", body[:200])
        return 1

    code, body = post("/cgi-bin/user_change_pass.cgi", {
        "old_password": INITIAL_PW,
        "new_password": REAL_PW,
        "csrf_token": csrf(),
    })
    print("[2] change password ->", code, body.strip()[:120])
    if '"ok"' not in body:
        print("    -> change FAILED")
        return 1

    code, body = post("/cgi-bin/login.cgi", {"user": "root", "pass": REAL_PW})
    print("[3] re-login with new password ->", code)
    try:
        r = op.open(urllib.request.Request(BASE + "/cgi-bin/main.cgi"), timeout=10)
        panel = r.read().decode(errors="replace")
        title = "control_panel" if "控制" in panel or "panel" in panel.lower() else "?"
        print("[4] main.cgi ->", r.status, "len", len(panel), "title-hint", title)
        ok = r.status == 200 and len(panel) > 2000
        print("[RESULT]", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    except urllib.error.HTTPError as e:
        print("[4] main.cgi ->", e.code)
        return 1


if __name__ == "__main__":
    sys.exit(main())

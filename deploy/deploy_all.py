"""deploy_all.py — one-click deployment driver for embeded-web on LubanCat.

Chains the foolproof-deploy.md Step1-Step8 flow (Step9 timing optional):
SSH bootstrap -> offline deps -> HTTPS phase -> CGI build -> panel password ->
system integration (+ time-wait-sync mask) -> verification 12/12 ->
(optional) --with-timing timing stack + NTP serving guard ->
(optional) --reboot re-test.

Every stage is idempotent: it detects what is already deployed and skips it,
so the driver can be re-run safely on a partially deployed board.  Board-side
work reuses the repo's proven scripts (build_on_board.sh, integrate_system.sh,
install_timing_stack.sh, fix_crlf_and_start.sh, enable_uart7_overlay.sh) —
nothing is reimplemented here.

Usage (PC, python 3.10+ with paramiko):
    python deploy/deploy_all.py                      # defaults below
    python deploy/deploy_all.py --with-timing        # + Step9 (needs UT986 wiring)
    python deploy/deploy_all.py --reboot             # + Step8 reboot re-test
Flags: --host --user --board-password --panel-password --force-build
"""
import argparse
import http.cookiejar
import json
import os
import sys
import tarfile
import time
import urllib.error
import urllib.parse
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REMOTE_DIR = "/tmp/deploy_all"


def ctx():
    import ssl
    return ssl._create_unverified_context()


def find(*cands):
    for c in cands:
        if c and os.path.exists(c):
            return c
    return None


def find_script(name):
    return find(os.path.join(REPO, "deploy", name),
                os.path.dirname(os.path.abspath(__file__)), name)


def find_debs():
    d = find(os.path.join(REPO, "debs"))
    if not d or not os.path.isdir(d):
        return []
    return [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith(".deb")]


class Driver:
    """One paramiko connection (RST-retry), sftp, exec helper."""

    def __init__(self, host, user, password):
        self.host, self.user, self.password = host, user, password
        self.client = None
        self.sftp = None

    def connect(self, tries=8):
        import paramiko
        last = None
        for i in range(tries):
            c = paramiko.SSHClient()
            c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
            try:
                c.connect(self.host, username=self.user, password=self.password,
                          look_for_keys=False, allow_agent=False,
                          timeout=10, banner_timeout=15)
                print("[ssh] connected (attempt %d)" % (i + 1))
                self.client, self.sftp = c, c.open_sftp()
                return True
            except paramiko.AuthenticationException as e:
                print("[-] auth failed: %s" % e)
                c.close()
                return False
            except (paramiko.SSHException, OSError) as e:
                print("[-] attempt %d: %s" % (i + 1, e))
                c.close()
                last = e
                time.sleep(2)
        print("[-] could not connect: %s" % last)
        return False

    def reconnect(self, wait_s=90, tries=12):
        print("[ssh] waiting %ds for the board…" % wait_s)
        time.sleep(wait_s)
        for _ in range(tries):
            time.sleep(10)
            if self.connect():
                return True
        return False

    def run(self, cmd, timeout=300):
        stdin, stdout, stderr = self.client.exec_command(cmd, timeout=timeout)
        out = stdout.read().decode(errors="replace")
        err = stderr.read().decode(errors="replace")
        rc = stdout.channel.recv_exit_status()
        return rc, out, err

    def upload(self, local, remote_name=None):
        try:
            self.sftp.stat(REMOTE_DIR)
        except OSError:
            self.sftp.mkdir(REMOTE_DIR, 0o755)
        name = remote_name or os.path.basename(local)
        self.sftp.put(local, REMOTE_DIR + "/" + name)


def sh(d, cmd, timeout=300, show=True):
    """Run a command, echo trimmed output, return (ok, full_output)."""
    rc, out, err = d.run(cmd, timeout)
    if show:
        if out.strip():
            print("  " + "\n  ".join(out.rstrip().splitlines()[:25]))
        if err.strip():
            print("  [stderr] " + "\n  ".join(err.rstrip().splitlines()[:6]))
    return rc == 0, out


# ------------------------------------------------------------------ stages
def stage_preflight(d, opt):
    _, out, _ = d.run("hostname; uname -m; cat /etc/debian_version 2>/dev/null; "
                      "gcc --version 2>/dev/null | head -1; nmcli --version 2>/dev/null; "
                      "id www-data && echo www-data-ok")
    print("  " + "\n  ".join(out.rstrip().splitlines()[:9]))
    return "aarch64" in out and "www-data-ok" in out


def stage_deps(d, opt):
    debs = find_debs()
    if len(debs) < 4:
        print("  [FAIL] debs/ 需 4 个离线 deb（lighttpd/mod-openssl/libxxhash0/chrony）")
        return False
    for f in debs:
        d.upload(f)
    _, out, _ = d.run("dpkg -s lighttpd chrony 2>/dev/null | "
                      "grep -c 'Status: install ok installed'")
    if out.strip() != "2":
        print("  [deps] installing offline debs…")
        d.run("dpkg -r ntp 2>/dev/null; "
              "dpkg -i /tmp/deploy_all/libxxhash0.deb /tmp/deploy_all/lighttpd.deb "
              "/tmp/deploy_all/lighttpd-mod-openssl.deb 2>&1 | tail -3")
        _, out, _ = d.run("dpkg -s chrony 2>/dev/null | grep ^Status || true")
        if "installed" not in out:
            d.run("dpkg -i /tmp/deploy_all/chrony.deb 2>&1 | tail -4")
    _, out, _ = d.run("dpkg -s lighttpd chrony 2>/dev/null | "
                      "grep -c 'Status: install ok installed'")
    ok = out.strip() == "2"
    print("  [deps] lighttpd+chrony installed=%s → %s" % (out.strip(), "OK" if ok else "FAIL"))
    return ok


def stage_https(d, opt):
    cfgd = find(os.path.join(REPO, "config"),
                os.path.join(REPO, "embeded-web-main", "config"))
    for f in ("lighttpd.conf", "10-cgi.conf", "10-ssl.conf"):
        d.upload(os.path.join(cfgd, f))
    d.run("mkdir -p /etc/lighttpd/conf-available /home/www/cgi-bin "
          "/var/cache/lighttpd/uploads /var/log/lighttpd && "
          "cp /tmp/deploy_all/lighttpd.conf /etc/lighttpd/lighttpd.conf && "
          "cp /tmp/deploy_all/10-cgi.conf /tmp/deploy_all/10-ssl.conf "
          "/etc/lighttpd/conf-available/ && "
          "rm -f /etc/lighttpd/conf-enabled/99-unconfigured.conf && "
          "ln -sf /etc/lighttpd/conf-available/10-cgi.conf /etc/lighttpd/conf-enabled/ && "
          "ln -sf /etc/lighttpd/conf-available/10-ssl.conf /etc/lighttpd/conf-enabled/ && "
          "chown -R www-data:www-data /home/www /var/cache/lighttpd /var/log/lighttpd")
    _, out, _ = d.run("[ -f /etc/lighttpd/server.pem ] && echo have || echo missing")
    if "missing" in out:
        d.run("openssl req -x509 -newkey rsa:2048 -keyout /etc/ssl/private/server.key "
              "-out /etc/ssl/certs/server.crt -days 3650 -nodes "
              "-subj '/CN=lubancat.local' 2>/dev/null && "
              "cat /etc/ssl/certs/server.crt /etc/ssl/private/server.key "
              "> /etc/lighttpd/server.pem && chmod 600 /etc/lighttpd/server.pem")
    rc, out, _ = d.run("lighttpd -tt -f /etc/lighttpd/lighttpd.conf >/dev/null 2>&1 && "
                       "echo SYNTAX-OK || echo SYNTAX-FAIL")
    if "SYNTAX-FAIL" in out:
        sh(d, "lighttpd -tt -f /etc/lighttpd/lighttpd.conf 2>&1 | head -6")
        return False
    d.run("systemctl enable --now lighttpd 2>&1 | tail -1; systemctl restart lighttpd")
    time.sleep(1)
    ok = False
    try:
        class NR(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, *a, **k):
                return None

        r = urllib.request.build_opener(NR).open(
            "http://%s/" % d.host, timeout=8)
        print("  [https] http / -> %s (no-follow)" % r.status)
        ok = r.status == 301
    except urllib.error.HTTPError as e:
        print("  [https] http / -> %s (redirect)" % e.code)
        ok = e.code in (301, 302)
    except Exception as e:
        print("  [https] FAIL: %s" % e)
    try:
        r2 = urllib.request.urlopen(
            urllib.request.Request("https://%s/" % d.host), timeout=8, context=ctx())
        print("  [https] https / -> %s" % r2.status)
    except urllib.error.HTTPError as e:
        print("  [https] https / -> %s" % e.code)
    return ok


def stage_cgi(d, opt):
    _, out, _ = d.run("[ -x /home/www/cgi-bin/main.cgi ] && echo present || echo absent")
    if "present" in out and not opt.force_build:
        print("  [cgi] already deployed — skip build (--force-build to rebuild)")
        return True
    srcdir = find(os.path.join(REPO, "src"),
                  os.path.join(REPO, "embeded-web-main", "src"))
    wwwdir = find(os.path.join(REPO, "www"),
                  os.path.join(REPO, "embeded-web-main", "www"))
    if not (srcdir and wwwdir and os.path.isdir(srcdir)):
        print("  [FAIL] src/ www/ 目录未找到（仓库布局或 embeded-web-main/ 布局）")
        return False
    tgz = os.path.join(REPO, "src.tar.gz")
    with tarfile.open(tgz, "w:gz") as tf:
        tf.add(srcdir, arcname="src")
    d.upload(tgz)
    for f in os.listdir(wwwdir):
        p = os.path.join(wwwdir, f)
        if os.path.isfile(p):
            d.upload(p)
    d.upload(find_script("build_on_board.sh"))
    ok, out = sh(d, "bash /tmp/deploy_all/build_on_board.sh 2>&1 | tail -20")
    print("  [cgi] %s" % ("OK" if "DEPLOY-OK" in out else "FAIL"))
    return "DEPLOY-OK" in out


def do_password(host, panel_pw):
    """Login flow: already-set → skip; fresh DB (root/admin) → forced change."""
    cj = http.cookiejar.CookieJar()

    class NR(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *a, **k):
            return None

    op = urllib.request.build_opener(
        urllib.request.HTTPCookieProcessor(cj),
        urllib.request.HTTPSHandler(context=ctx()), NR)
    base = "https://%s" % host
    for pw in (panel_pw, "admin"):
        try:
            op.open(urllib.request.Request(
                base + "/cgi-bin/login.cgi",
                data=urllib.parse.urlencode({"user": "root", "pass": pw}).encode()),
                timeout=10)
            # followed redirect → session up with this password already
            print("  [password] login as %s → session OK" %
                  ("panel" if pw == panel_pw else "initial"))
            if pw == panel_pw:
                return True
            return False  # admin session but panel password differs: user must set
        except urllib.error.HTTPError as e:
            loc = dict(e.headers).get("Location", "")
            if "main.cgi" in loc:
                print("  [password] already set — skip")
                return True
            if "change" in loc:
                print("  [password] first login as %s → forced change" % pw)
                csrf = next((c.value for c in cj if c.name == "csrf_token"), "")
                r = op.open(urllib.request.Request(
                    base + "/cgi-bin/user_change_pass.cgi",
                    data=urllib.parse.urlencode({
                        "old_password": pw,
                        "new_password": panel_pw,
                        "csrf_token": csrf}).encode()), timeout=10)
                body = r.read().decode(errors="replace")
                print("  [password] change →", body.strip()[:100])
                if '"ok"' in body:
                    print("  [password] panel password now set — record it!")
                    return True
                return False
            print("  [password] rejected (%s): %s %s" % (pw, e.code, loc))
    print("  [password] FAIL — panel/初始密码都无法登录")
    return False


def stage_password(d, opt):
    return do_password(d.host, opt.panel_password)


def stage_integrate(d, opt):
    _, out, _ = d.run("[ -f /etc/sudoers.d/99-www-nmcli ] && "
                      "[ -x /usr/local/bin/ntp_nic_monitor ] && "
                      "echo present || echo missing")
    if "missing" in out:
        for f in ("chrony_acl_apply.sh", "ntp_stats_sample.sh", "ntp_nic_monitor.c",
                  "ntp-stats.service", "ntp-stats.timer", "ntp-nic-monitor.service",
                  "rollback_watchdog.sh", "rollback-recover.service"):
            d.upload(find(os.path.join(REPO, f)))
        d.upload(find_script("integrate_system.sh"))
        ok, out = sh(d, "bash /tmp/deploy_all/integrate_system.sh 2>&1 | tail -6")
        if "INTEGRATION-OK" not in out:
            print("  [integrate] FAIL")
            return False
    d.run("systemctl disable --now systemd-time-wait-sync 2>/dev/null; "
          "systemctl mask systemd-time-wait-sync 2>&1 | tail -1")
    _, out, _ = d.run("systemctl is-active lighttpd chrony ntp-nic-monitor "
                      "ntp-stats.timer; systemctl list-jobs --no-pager | head -2")
    ok = out.count("active") >= 4
    print("  [integrate] services %s" % ("OK" if ok else "CHECK:\n" + out))
    return ok


def stage_timing(d, opt):
    _, out, _ = d.run("[ -e /dev/ttyS7 ] && echo yes || echo no")
    if "no" in out:
        print("  [timing] /dev/ttyS7 缺失 → 启用 uart7 overlay 并重启板卡…")
        d.upload(find_script("enable_uart7_overlay.sh"))
        d.run("bash /tmp/deploy_all/enable_uart7_overlay.sh >/dev/null 2>&1; "
              "sync; (sleep 2; reboot) >/dev/null 2>&1 &")
        if not d.reconnect():
            print("  [timing] board did not come back")
            return False
        _, out, _ = d.run("[ -e /dev/ttyS7 ] && echo yes || echo no")
        if "no" in out:
            print("  [timing] overlay 已应用但 /dev/ttyS7 仍缺失 — 检查 uEnv")
            return False
    d.upload(find_script("install_timing_stack.sh"))
    d.upload(find_script("fix_crlf_and_start.sh"))
    sh(d, "bash /tmp/deploy_all/install_timing_stack.sh 2>&1 | tail -8", show=False)
    sh(d, "bash /tmp/deploy_all/fix_crlf_and_start.sh 2>&1 | tail -4", show=False)
    # NTP serving guard: board watchdog must match the repo's script+conf
    import hashlib
    wd = os.path.join(REPO, "pps_tod", "pps_tod_watchdog.sh")
    cf = os.path.join(REPO, "pps_tod", "pps_tod_watchdog.conf")
    for local, target, remote_name, svc in (
            (wd, "/usr/local/bin/pps_tod_watchdog.sh", "wd_guard.sh",
             "pps_tod_watchdog"),
            (cf, "/etc/pps_tod/pps_tod_watchdog.conf", "wd_conf.conf",
             "pps_tod_watchdog")):
        if not os.path.isfile(local):
            continue
        h = hashlib.md5(open(local, "rb").read()).hexdigest()
        _, out, _ = d.run("md5sum %s 2>/dev/null" % target)
        if not out.startswith(h):
            print("  [guard] %s differs — deploying repo version…" % target)
            d.upload(local, remote_name=remote_name)
            d.run("sed -i 's/\\r$//' /tmp/deploy_all/%s && "
                  "sh -n /tmp/deploy_all/%s && "
                  "cp /tmp/deploy_all/%s %s && "
                  "chmod 755 %s && systemctl restart %s 2>&1 | tail -1"
                  % (remote_name, remote_name, remote_name, target, target, svc))
    _, out, _ = d.run("systemctl is-active pps_tod pps_tod_watchdog chrony; "
                      "cat /run/pps_tod/status /run/pps_tod/watchdog.state 2>/dev/null")
    print("  " + "\n  ".join(out.rstrip().splitlines()[:22]))
    ok = "good=1" in out and "ntp_serving=on" in out
    print("  [timing] %s（无 UT986 硬件时 sources 不可达、good=0 属预期，不阻塞）" %
          ("OK" if ok else "NOT-LOCKED（见上方状态）"))
    return ok


def stage_verify(d, opt):
    import verify_functions as vf
    vf.BASE = "https://%s" % d.host
    vf.USER = "root"
    vf.PASS = opt.panel_password
    rc = vf.main()
    return rc == 0


def stage_reboot_retest(d, opt):
    d.run("md5sum /etc/ssh/ssh_host_ed25519_key /etc/lighttpd/lighttpd.conf "
          "/var/db/myapp.db /home/www/cgi-bin/main.cgi /root/.ssh/authorized_keys "
          "> /var/db/md5_baseline.txt; sync; (sleep 2; reboot) >/dev/null 2>&1 &")
    print("  reboot issued…")
    if not d.reconnect():
        print("  [reboot] board did not come back")
        return False
    ok, out = sh(d, "uptime; systemctl is-active lighttpd chrony ntp-nic-monitor "
                    "ntp-stats.timer; md5sum -c /var/db/md5_baseline.txt 2>&1 | tail -7")
    ok = "FAILED" not in out.upper() and out.count("active") >= 4
    print("  [reboot] %s（myapp.db 差异属预期）" % ("OK" if ok else "CHECK"))
    return ok


STAGES = {
    "preflight": stage_preflight,
    "deps": stage_deps,
    "https": stage_https,
    "cgi": stage_cgi,
    "password": stage_password,
    "integrate": stage_integrate,
    "timing": stage_timing,
    "verify": stage_verify,
    "reboot-retest": stage_reboot_retest,
}


def main():
    ap = argparse.ArgumentParser(description="one-click deploy driver (idempotent)")
    ap.add_argument("--host", default="192.168.1.111")
    ap.add_argument("--user", default="root")
    ap.add_argument("--board-password", default="root")
    ap.add_argument("--panel-password", default="Testpassword1234@@")
    ap.add_argument("--with-timing", action="store_true",
                    help="Step9 授时链路（需 UT986 接线）")
    ap.add_argument("--reboot", action="store_true",
                    help="Step8 断电重启复测")
    ap.add_argument("--force-build", action="store_true",
                    help="强制重编 CGI（默认已部署则跳过）")
    opt = ap.parse_args()

    d = Driver(opt.host, opt.user, opt.board_password)
    if not d.connect():
        print("[FAIL] SSH 不通 — 全新板卡先在板上跑一次: "
              "ssh-keygen -A && mkdir -p /run/sshd && sshd -t（详见手册 §2）")
        return 1

    order = ["preflight", "deps", "https", "cgi", "password", "integrate"]
    if opt.with_timing:
        order.append("timing")
    order.append("verify")
    if opt.reboot:
        order.append("reboot-retest")

    results = []
    for name in order:
        print("\n===== stage: %s =====" % name)
        try:
            ok = STAGES[name](d, opt)
        except Exception:
            import traceback
            traceback.print_exc()
            ok = False
        results.append((name, ok))
        if not ok:
            print("\n[ABORT] 阶段 '%s' 失败 —— 修复后重跑本脚本（幂等，已完成阶段自动跳过）；"
                  "手动分步与排障见 docs/foolproof-deploy.md" % name)
            break

    print("\n===== DEPLOY SUMMARY =====")
    for name, ok in results:
        print("[%s] %s" % ("PASS" if ok else "FAIL", name))
    print("%d/%d stages passed" % (sum(1 for _, ok in results if ok), len(results)))
    return 0 if all(ok for _, ok in results) else 1


if __name__ == "__main__":
    sys.exit(main())

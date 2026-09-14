#!/usr/bin/env python3
"""test_frontend.py — front-end NIC registry consistency checks (offline).

The set of manageable NICs lives in two tables by design:

  * src/nmcli.c g_nics        — which interfaces network.cgi accepts
  * www/control_panel.html    — JS NICS registry (tabs, port param)

They share exactly one field — the NIC name (eth0..eth3). This test
asserts they stay in sync, both directions and in order.  Order matters:
the tabs render in registry order and eth0 must stay first — it is the
only NIC whose changes arm the rollback watchdog (ADR-0003 D10).

Rendering itself is JS-only and cannot be checked statically — browser
screenshots cover that. This test only guards the data contract, so
adding a NIC to one table but not the other fails immediately.

Usage: ./test_frontend.py [repo_root]   (default: script's parent dir)
Exit: 0 = consistent, 1 = drift found (each drift printed)
"""
import re
import sys
from pathlib import Path

# C table: static const char *g_nics[4] = { "eth0", "eth1", "eth2", "eth3" };
C_NICS_RE = re.compile(r"g_nics\[[^\]]*\]\s*=\s*\{\s*([^}]*)\}")
C_NIC_RE = re.compile(r'"([^"]+)"')
# JS table: var NICS = [ { nic: 'eth0' }, { nic: 'eth1' }, ... ];
JS_NICS_RE = re.compile(r"var\s+NICS\s*=\s*\[(.*?)\];", re.S)
JS_NIC_RE = re.compile(r"nic\s*:\s*'([^']+)'")


def parse_c_nics(src_text):
    m = C_NICS_RE.search(src_text)
    if not m:
        raise ValueError("g_nics array not found in src/nmcli.c")
    return C_NIC_RE.findall(m.group(1))


def parse_js_nics(html_text):
    m = JS_NICS_RE.search(html_text)
    if not m:
        raise ValueError("NICS array not found in www/control_panel.html")
    return JS_NIC_RE.findall(m.group(1))


# Time-sync tab contract: the front-end polls timesync.cgi?action=status
# and posts action=setmode; the CGI must handle exactly those actions.
# Same two-sided-registry idea as the NIC check.
TS_ACTIONS = ("status", "setmode")


def check_timesync(root, errors):
    html = (root / "www" / "control_panel.html").read_text()
    cgi = (root / "src" / "timesync.cgi.c").read_text()
    for action in TS_ACTIONS:
        if "/cgi-bin/timesync.cgi?action=%s" % action not in html:
            errors.append("front-end missing timesync.cgi?action=%s" % action)
        if 'strcmp(action, "%s") == 0' % action not in cgi:
            errors.append("timesync.cgi missing handler for action=%s" % action)


def check(repo):
    root = Path(repo)
    c_nics = parse_c_nics((root / "src" / "nmcli.c").read_text())
    js_nics = parse_js_nics((root / "www" / "control_panel.html").read_text())

    errors = []
    check_timesync(root, errors)
    if c_nics != js_nics:
        errors.append("NIC lists drifted: C=%s JS=%s" % (c_nics, js_nics))
    if not js_nics:
        errors.append("JS NICS registry is empty")
    if js_nics and js_nics[0] != "eth0":
        errors.append("first NIC must be eth0 (rollback watchdog port): %s"
                      % js_nics[0])
    if len(set(js_nics)) != len(js_nics):
        errors.append("duplicate NIC entries: %s" % js_nics)

    if errors:
        for e in errors:
            print("  ❌ %s" % e)
        print("frontend NICS registry ↔ nmcli.c g_nics out of sync")
        return 1
    print("  ✅ frontend NICS registry consistent with nmcli.c g_nics "
          "(%d NICs)" % len(js_nics))
    return 0


if __name__ == "__main__":
    sys.exit(check(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).parent))

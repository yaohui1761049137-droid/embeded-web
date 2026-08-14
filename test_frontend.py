#!/usr/bin/env python3
"""test_frontend.py — front-end board registry consistency checks (offline).

The "Board" concept lives in two tables by design:

  * src/remote.c g_boards     — protocol routing (port key → device/baud)
  * www/control_panel.html    — JS BOARDS registry (tabs, port param,
                                placeholder boards)

They share exactly one field — the port key. This test asserts they stay
in sync:

  * C table port keys == port keys of supported (ok) JS boards (both ways)
  * every supported JS board carries port + dev; dev matches the C device
  * unsupported JS boards carry no port key
  * board ids are unique; every board has an id + label

Rendering itself is JS-only and cannot be checked statically — browser
screenshots cover that. This test only guards the data contract, so
adding a board to one table but not the other fails immediately.

Usage: ./test_frontend.py [repo_root]   (default: script's parent dir)
Exit: 0 = consistent, 1 = drift found (each drift printed)
"""
import re
import sys
from pathlib import Path

# C table rows: { "<port>", "/dev/<dev>", <baud> }
C_ROW_RE = re.compile(r'\{\s*"([^"]*)",\s*"/dev/([^"]+)",\s*[0-9]+\s*\}')
# JS entry fields: key : 'str' | number | true|false
JS_FIELD_RE = re.compile(r"(\w+)\s*:\s*(?:'([^']*)'|(\d+)|(true|false))")


def parse_c_boards(src_text):
    return [(m.group(1), m.group(2)) for m in C_ROW_RE.finditer(src_text)]


def parse_js_boards(html_text):
    m = re.search(r"var\s+BOARDS\s*=\s*\[(.*?)\];", html_text, re.S)
    if not m:
        raise ValueError("BOARDS array not found in www/control_panel.html")
    boards = []
    for block in re.finditer(r"\{([^{}]*)\}", m.group(1)):
        b = {}
        for fm in JS_FIELD_RE.finditer(block.group(1)):
            key = fm.group(1)
            if fm.group(2) is not None:
                b[key] = fm.group(2)    # quoted string; '' is valid (Board 1 port)
            elif fm.group(3):
                b[key] = int(fm.group(3))
            elif fm.group(4):
                b[key] = fm.group(4) == "true"
        boards.append(b)
    return boards


def check(repo):
    root = Path(repo)
    c_rows = parse_c_boards((root / "src" / "remote.c").read_text())
    boards = parse_js_boards((root / "www" / "control_panel.html").read_text())

    errors = []
    c_ports = {p for p, _ in c_rows}
    c_dev = {p: d for p, d in c_rows}
    ok_boards = [b for b in boards if b.get("ok")]
    js_ports = {b["port"] for b in ok_boards if "port" in b}

    if c_ports != js_ports:
        errors.append("port keys drifted: "
                      "C=%s JS(ok)=%s" % (sorted(c_ports), sorted(js_ports)))

    for b in ok_boards:
        if "port" not in b or "dev" not in b:
            errors.append("Board %s: supported board lacks port/dev fields"
                          % b.get("id"))
            continue
        cdev = c_dev.get(b["port"])
        if cdev is None:
            continue  # already reported by the set mismatch above
        if b["dev"] != cdev:
            errors.append("Board %s (port=%s): dev '%s' != C device '%s'"
                          % (b.get("id"), b["port"], b["dev"], cdev))

    for b in boards:
        if not b.get("ok") and "port" in b:
            errors.append("Board %s: unsupported board must not carry a port key"
                          % b.get("id"))
        if "id" not in b or "label" not in b:
            errors.append("Board %s: every board needs id + label" % b.get("id"))

    ids = [b.get("id") for b in boards]
    if len(set(ids)) != len(ids):
        errors.append("duplicate board ids: %s" % ids)

    if errors:
        for e in errors:
            print("  ❌ %s" % e)
        print("frontend registry ↔ C board table out of sync")
        return 1
    print("  ✅ frontend registry consistent with remote.c "
          "(%d supported / %d slots)" % (len(ok_boards), len(boards)))
    return 0


if __name__ == "__main__":
    sys.exit(check(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).parent))

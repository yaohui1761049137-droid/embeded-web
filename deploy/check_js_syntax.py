"""Syntax-check every <script> block in control_panel.html with esprima."""
import re
import esprima

html = open(r"C:\Users\YAO\Desktop\codex_store\without_HDMI_test\embeded-web-main\www\control_panel.html",
            encoding="utf-8").read()
blocks = re.findall(r"<script>(.*?)</script>", html, re.S)
print("script blocks:", len(blocks))
ok = True
for i, b in enumerate(blocks):
    try:
        esprima.parseScript(b)
        print("block %d: syntax OK (%d chars)" % (i, len(b)))
    except Exception as e:
        ok = False
        print("block %d SYNTAX ERROR: %s" % (i + 1, e))
print("RESULT:", "SYNTAX-OK" if ok else "SYNTAX-ERROR")

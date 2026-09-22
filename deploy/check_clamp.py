"""Confirm the rate clamp is served by the board."""
import ssl
import urllib.request

ctx = ssl._create_unverified_context()
page = urllib.request.urlopen(
    urllib.request.Request("https://192.168.1.111/control_panel.html"),
    context=ctx, timeout=10).read().decode("utf-8", errors="replace")
approx = chr(0x2248) + "100%"           # ≈100%
print("rateTxt in served page:", "rateTxt" in page)
print("clamp branch present:", approx in page)

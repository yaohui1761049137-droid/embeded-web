"""Verify the served control_panel.html contains both changes."""
import ssl
import urllib.request

ctx = ssl._create_unverified_context()
page = urllib.request.urlopen(
    urllib.request.Request("https://192.168.1.111/control_panel.html"),
    context=ctx, timeout=10).read().decode("utf-8", errors="replace")

checks = {
    "fmtNum keeps one decimal for fractional ticks":
        "if (v < 100 && v !== Math.round(v))" in page,
    "new row: receiver fix (ts-rx-fix)": 'id="ts-rx-fix"' in page,
    "new row: satellite count (ts-rx-sats)": 'id="ts-rx-sats"' in page,
    "renderTimesync reads p.sats": "tsSet('ts-rx-sats'" in page,
    "fix code 7 mapped": "'7': '定位有效 (码 7)'" in page,
    "sats staleness guard (sats_age_s)": "sats_age_s" in page,
}
for k, v in checks.items():
    print(("[OK]  " if v else "[MISS]") + " " + k)
print("RESULT:", "ALL-PASS" if all(checks.values()) else "FAIL")

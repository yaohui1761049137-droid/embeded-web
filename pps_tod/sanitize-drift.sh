#!/bin/sh
# Reset chrony drift if implausible (|ppm| > 5000) - prevents a garbage-era
# frequency estimate from being re-applied after reboot
F=/var/lib/chrony/chrony.drift
if [ -f "$F" ]; then
  BAD=$(awk 'NR==1{ a=($1<0)?-($1+0):($1+0); if (a>5000) print 1; else print 0 }' "$F")
  if [ "$BAD" = "1" ]; then
    rm -f "$F"
    echo "$(date) sanitized drift" >> /tmp/sanitize-drift.log
  fi
fi
exit 0

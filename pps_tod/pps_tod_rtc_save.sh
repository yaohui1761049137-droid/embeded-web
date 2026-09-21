#!/bin/sh
# persist the synchronized system time to the RTC
# (run by pps_tod_rtc.timer every 6h; the original /etc/cron.d entry never
#  fired because the cron package is not installed on this image)
if ! /usr/sbin/hwclock -w; then
    logger -t pps_tod_rtc "hwclock -w failed with exit $?"
fi
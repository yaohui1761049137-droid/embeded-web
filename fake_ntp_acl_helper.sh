#!/bin/bash
# Fake chrony_acl_apply.sh for offline host tests (NTPMON_HELPER_OVERRIDE).
# Logs every invocation (for assertions); FAKE_ACL_FAIL=1 simulates a
# helper failure with a stderr message.
STATE="${FAKE_ACL_STATE:-/tmp/ntpmon_test}"
mkdir -p "$STATE"
echo "$*" >> "$STATE/calls.log"
if [ -n "$FAKE_ACL_FAIL" ]; then
    echo "错误: 模拟失败(测试)" >&2
    exit 1
fi
echo "OK: fake $*"
exit 0

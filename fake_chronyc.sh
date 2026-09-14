#!/bin/bash
# Fake chronyc for offline host tests (CHRONYC_OVERRIDE).
# Mirrors the real output shape of the board's chrony 3.x (Debian
# Buster): sources CSV has separate flags/state columns and a decimal
# last sample; tracking has 14 columns (extra "Reference Name").
# One chrony-4-style row (merged MS column, hex-encoded last sample)
# is included to exercise the layout fallback and hex decoding.
if [ "$1" = "-c" ]; then
    case "$2" in
    sources)
        cat <<'EOF'
#,*,PPS,0,2,377,3,-0.000018280,-0.000019961,0.025022149
^- ,ntp.example.net,1,10,377,16,2B343675735B202D313275735D202B2F2D20326D73
EOF
        ;;
    tracking)
        cat <<'EOF'
50505300,PPS,1,1789358256.266601122,0.000001438,-0.000001697,0.000002959,-18.531,-0.004,0.251,0.050000001,0.000032273,4.0,Normal
EOF
        ;;
    *)
        echo "fake-chronyc: unhandled: $*" >&2
        exit 1
        ;;
    esac
    exit 0
fi
echo "fake-chronyc: unhandled: $*" >&2
exit 1
#!/bin/bash
# 0133: touch: read whole THP frames (report 0xc0, ~1.5 KB), then turn them off and turn
# the firmware's own touch reports (0x11) on; IC reset after 20 read errors in a row.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
W=/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl
tr -d '\r' < $W/pandora_touch.c > bsd/dev/pandora_touch.c
git add -A
git diff --cached --stat
git diff --cached > $W/../xnu/patches/0133-pandora-touch-thp-off-touch-reports-on.patch
git -c user.name=t -c user.email=t@t commit -qm 0133

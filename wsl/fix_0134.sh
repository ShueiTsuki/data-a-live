#!/bin/bash
# 0134: touch: optional fingers from THP frames (pandora_touch bit 3), SPI at 9.6 MHz.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
W=/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl
tr -d '\r' < $W/pandora_touch.c > bsd/dev/pandora_touch.c
git add -A
git diff --cached --stat
git diff --cached > $W/../xnu/patches/0134-pandora-touch-thp-fallback.patch
git -c user.name=t -c user.email=t@t commit -qm 0134

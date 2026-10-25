#!/bin/bash
# 0214: date and time from the PMK8850 RTC plus Android's offset (boot-arg pandora_rtc_offset).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0214.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0214-pandora-rtc.patch
git -c user.name=t -c user.email=t@t commit -qm 0214

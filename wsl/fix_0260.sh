#!/bin/bash
# 0260: battery and NAND sensors for iOS.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0260.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0260-therm-ios-battery.patch
git -c user.name=t -c user.email=t@t commit -qm 0260

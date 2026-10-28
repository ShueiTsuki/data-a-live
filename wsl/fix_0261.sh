#!/bin/bash
# 0261: iOS temperatures are 16.16.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0261.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0261-therm-ios-fixed.patch
git -c user.name=t -c user.email=t@t commit -qm 0261

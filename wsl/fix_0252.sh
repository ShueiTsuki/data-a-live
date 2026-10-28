#!/bin/bash
# 0252: thermal-monitor setup (SPMI writes).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0252.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0252-therm-tm-setup.patch
git -c user.name=t -c user.email=t@t commit -qm 0252

#!/bin/bash
# 0215: cores idle in the DT's retention state; the display controller read once at boot.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0215.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0215-pandora-idle-dpu-probe.patch
git -c user.name=t -c user.email=t@t commit -qm 0215

#!/bin/bash
# 0230: the real battery from the SOCCP.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0230.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0230-soccp-battery.patch
git -c user.name=t -c user.email=t@t commit -qm 0230

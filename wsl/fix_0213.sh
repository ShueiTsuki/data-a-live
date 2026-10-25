#!/bin/bash
# 0213: thermal control from the DT thermal zones (CPU and GPU level caps, hot cores out of the scheduler).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0213.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0213-pandora-thermal.patch
git -c user.name=t -c user.email=t@t commit -qm 0213

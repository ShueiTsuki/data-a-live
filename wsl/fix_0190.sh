#!/bin/bash
# 0190: GPU watch for 10 minutes, SMMU FSR logged on change.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0190.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0190-pandora-gpu-watch-long.patch
git -c user.name=t -c user.email=t@t commit -qm 0190

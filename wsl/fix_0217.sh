#!/bin/bash
# 0217: IOSurface use counts; the display scans out iOS's frames itself (SMMU bank, pipes, scaler).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0217.py
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0217b.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0217-pandora-scanout.patch
git -c user.name=t -c user.email=t@t commit -qm 0217

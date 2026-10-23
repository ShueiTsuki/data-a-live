#!/bin/bash
# 0179: Adreno 840 carved GPU pool mapped in windows, not as a whole.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0179.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0179-pandora-gpu-pool-windows.patch
git -c user.name=t -c user.email=t@t commit -qm 0179

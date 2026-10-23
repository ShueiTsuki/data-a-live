#!/bin/bash
# 0180: Adreno 840 carved GPU pool windows that can be unmapped.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0180.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0180-pandora-gpu-pool-unmappable.patch
git -c user.name=t -c user.email=t@t commit -qm 0180

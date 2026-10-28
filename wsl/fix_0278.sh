#!/bin/bash
# 0278: amp 0x0104/0x0124 left at 1, Android start order, register dump while playing.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0278.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0278-amp-chip-values.patch
git -c user.name=t -c user.email=t@t commit -qm 0278

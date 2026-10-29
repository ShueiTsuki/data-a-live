#!/bin/bash
# 0325: the IB1 of a GPU hang walked as it is in memory.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0325.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0325-gpu-hang-walk.patch
git -c user.name=t -c user.email=t@t commit -qm 0325

#!/bin/bash
# 0219: 4 GiB GPU address windows (36-bit GPU context), as msm private address spaces.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0219.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0219-pandora-gpu-windows.patch
git -c user.name=t -c user.email=t@t commit -qm 0219

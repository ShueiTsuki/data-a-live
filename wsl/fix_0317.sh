#!/bin/bash
# 0317: pandora_stacks takes every process of a name; returns 8 to a line.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0317.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0317-stacks-all-processes.patch
git -c user.name=t -c user.email=t@t commit -qm 0317

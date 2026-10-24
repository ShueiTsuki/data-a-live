#!/bin/bash
# 0202: Governor floor, big-core control tries, per-CPU busyness.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0202.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0202-pandora-cpu-governor2.patch
git -c user.name=t -c user.email=t@t commit -qm 0202

#!/bin/bash
# 0223: call stacks in the sampling profiler.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0223.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0223-pandora-prof-stacks.patch
git -c user.name=t -c user.email=t@t commit -qm 0223

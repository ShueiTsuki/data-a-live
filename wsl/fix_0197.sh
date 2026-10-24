#!/bin/bash
# 0197: GPU frequency scaling (busy counter, simple_ondemand).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0197.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0197-pandora-gpu-dcvs.patch
git -c user.name=t -c user.email=t@t commit -qm 0197

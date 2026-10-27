#!/bin/bash
# 0244: log the first periodic gauge read.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0244.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0244-gauge-first-log.patch
git -c user.name=t -c user.email=t@t commit -qm 0244

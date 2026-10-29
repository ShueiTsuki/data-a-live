#!/bin/bash
# 0305: the levels the DSP gets after each start.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0305.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0305-audio-start-levels.patch
git -c user.name=t -c user.email=t@t commit -qm 0305

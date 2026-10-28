#!/bin/bash
# 0279: bottom 500 Hz, top 1 kHz, then both.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0279.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0279-speakers-apart.patch
git -c user.name=t -c user.email=t@t commit -qm 0279

#!/bin/bash
# 0266: the amps' ROM stopped right after the reset.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0266.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0266-amp-rom-stop-early.patch
git -c user.name=t -c user.email=t@t commit -qm 0266

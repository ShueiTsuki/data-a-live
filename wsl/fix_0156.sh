#!/bin/bash
# 0156: PANDORA stops drawing the kernel text over the GUI from the first frame on.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0156.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0156-pandora-console-off-at-logo.patch
git -c user.name=t -c user.email=t@t commit -qm 0156

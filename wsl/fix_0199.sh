#!/bin/bash
# 0199: IOSurface sizes of 0 mean "the kernel picks" ('b3a8' planes, the App Library); fence wait timeouts split.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0199.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0199-pandora-surface-row-zero.patch
git -c user.name=t -c user.email=t@t commit -qm 0199

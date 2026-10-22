#!/bin/bash
# 0157: mute the console at the first swap that shows a surface, not at an empty one.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0157.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0157-pandora-console-off-first-image.patch
git -c user.name=t -c user.email=t@t commit -qm 0157

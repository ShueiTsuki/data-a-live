#!/bin/bash
# 0264: the panel's own dimming off by default.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0264.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0264-panel-dimming-off.patch
git -c user.name=t -c user.email=t@t commit -qm 0264

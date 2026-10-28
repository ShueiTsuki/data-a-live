#!/bin/bash
# 0287: audio device setProperties (hog mode).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0287.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0287-audio-setproperties.patch
git -c user.name=t -c user.email=t@t commit -qm 0287

#!/bin/bash
# 0290: audio device published 3 s after the ringer read.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0290.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0290-audio-after-ringer.patch
git -c user.name=t -c user.email=t@t commit -qm 0290

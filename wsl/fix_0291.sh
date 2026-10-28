#!/bin/bash
# 0291: Thunderbolt audio device UID.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0291.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0291-audio-tt-uid.patch
git -c user.name=t -c user.email=t@t commit -qm 0291

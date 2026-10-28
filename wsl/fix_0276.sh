#!/bin/bash
# 0276: the speaker graph's stream volume control at unity, unmuted.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0276.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0276-stream-volume-unity.patch
git -c user.name=t -c user.email=t@t commit -qm 0276

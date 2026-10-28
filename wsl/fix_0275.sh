#!/bin/bash
# 0275: louder, longer test beeps; the amps' playback readouts.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0275.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0275-beeps-readouts.patch
git -c user.name=t -c user.email=t@t commit -qm 0275

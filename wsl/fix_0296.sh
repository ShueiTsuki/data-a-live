#!/bin/bash
# 0296: live play logs less.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0296.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0296-audio-live-log.patch
git -c user.name=t -c user.email=t@t commit -qm 0296

#!/bin/bash
# 0160: SwapEnd waits until the previous frame has been copied (no half-drawn frames).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0160.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0160-pandora-swap-waits-for-copy.patch
git -c user.name=t -c user.email=t@t commit -qm 0160

#!/bin/bash
# 0204: IOSurface plane rows at the offset IOSurface.framework reads; CPU governor without the limits cap.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0204.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0204-pandora-plane-rows.patch
git -c user.name=t -c user.email=t@t commit -qm 0204

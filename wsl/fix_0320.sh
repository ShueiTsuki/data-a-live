#!/bin/bash
# 0320: two clusters (AMP, Edge scheduler) and P-spill thread group placement.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0320.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0320-amp-pspill.patch
git -c user.name=t -c user.email=t@t commit -qm 0320

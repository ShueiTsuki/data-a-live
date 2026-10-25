#!/bin/bash
# 0225: the profiler logs the image list with every report.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0225.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0225-prof-images-every-report.patch
git -c user.name=t -c user.email=t@t commit -qm 0225

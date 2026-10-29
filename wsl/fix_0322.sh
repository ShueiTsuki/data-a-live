#!/bin/bash
# 0322: work interval instances (work_interval_ctl operations 6-8) to the performance controller.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0322.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0322-work-interval-instances.patch
git -c user.name=t -c user.email=t@t commit -qm 0322

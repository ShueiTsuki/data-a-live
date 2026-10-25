#!/bin/bash
# 0227: one SCM call at a time, queued calls resumed.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0227.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0227-scm-lock-waitq.patch
git -c user.name=t -c user.email=t@t commit -qm 0227

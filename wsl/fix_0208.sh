#!/bin/bash
# 0208: every persistent-log line starts with the time since boot ([s.ms]), to see where boot time goes.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0208.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0208-pandora-log-timestamps.patch
git -c user.name=t -c user.email=t@t commit -qm 0208

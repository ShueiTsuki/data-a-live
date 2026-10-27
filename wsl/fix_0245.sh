#!/bin/bash
# 0245: plugging and unplugging show at once.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0245.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0245-plug-at-once.patch
git -c user.name=t -c user.email=t@t commit -qm 0245

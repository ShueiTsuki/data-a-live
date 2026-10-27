#!/bin/bash
# 0241: the gauge read as Android sends it.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0241.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0241-gauge-as-android.patch
git -c user.name=t -c user.email=t@t commit -qm 0241

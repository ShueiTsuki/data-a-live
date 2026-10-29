#!/bin/bash
# 0309: a screen shot on any two keys.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0309.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0309-shot-any-two-keys.patch
git -c user.name=t -c user.email=t@t commit -qm 0309

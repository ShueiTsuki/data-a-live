#!/bin/bash
# 0194: 128 IBs per submission (no Turnip split), IB2/IB3 and IB words in the hang dump.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0194.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0194-pandora-gpu-128-ibs.patch
git -c user.name=t -c user.email=t@t commit -qm 0194

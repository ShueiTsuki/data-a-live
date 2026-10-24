#!/bin/bash
# 0192: show a GPU frame only once the GPU finished it.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0192.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0192-pandora-fb-wait-gpu.patch
git -c user.name=t -c user.email=t@t commit -qm 0192

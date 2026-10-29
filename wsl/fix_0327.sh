#!/bin/bash
# 0327: IBs whose buffer handle is no longer live.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0327.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0327-gpu-stale-handles.patch
git -c user.name=t -c user.email=t@t commit -qm 0327

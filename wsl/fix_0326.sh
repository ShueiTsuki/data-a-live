#!/bin/bash
# 0326: IB memory written while the GPU can still read it.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0326.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0326-gpu-ib-reuse.patch
git -c user.name=t -c user.email=t@t commit -qm 0326

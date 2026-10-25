#!/bin/bash
# 0216: the GIC as IOKit's interrupt controller; the GPU's fence interrupt wakes the waiters.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0216.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0216-pandora-gic-gpu-interrupt.patch
git -c user.name=t -c user.email=t@t commit -qm 0216

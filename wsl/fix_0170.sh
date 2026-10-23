#!/bin/bash
# 0170: Adreno 840 bring-up level 8 in Linux order: GX vote before OOB GPU_SET.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0170.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0170-pandora-gpu-gx-vote-first.patch
git -c user.name=t -c user.email=t@t commit -qm 0170

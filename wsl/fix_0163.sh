#!/bin/bash
# 0163: Adreno 840 bring-up level 2, CX power and SMMU clock (pandora_gpu=2).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0163.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0163-pandora-gpu-level2.patch
git -c user.name=t -c user.email=t@t commit -qm 0163

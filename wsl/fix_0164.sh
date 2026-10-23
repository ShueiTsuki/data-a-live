#!/bin/bash
# 0164: Adreno 840 bring-up level 3, GPLL0 vote and GPU_INIT_REGS (pandora_gpu=3).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0164.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0164-pandora-gpu-level3.patch
git -c user.name=t -c user.email=t@t commit -qm 0164

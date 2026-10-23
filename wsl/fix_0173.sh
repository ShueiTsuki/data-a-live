#!/bin/bash
# 0173: Adreno 840 bring-up level 11, zap shader and secure mode off (pandora_gpu=11).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0173.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0173-pandora-gpu-zap.patch
git -c user.name=t -c user.email=t@t commit -qm 0173

#!/bin/bash
# 0171: Adreno 840 bring-up level 9, the GPU SMMU context and SQE (pandora_gpu=9).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0171.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0171-pandora-gpu-smmu-sqe.patch
git -c user.name=t -c user.email=t@t commit -qm 0171

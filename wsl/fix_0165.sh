#!/bin/bash
# 0165: Adreno 840 bring-up level 4a, the GMU SMMU context (pandora_gpu=4).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0165.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0165-pandora-gpu-gmu-smmu.patch
git -c user.name=t -c user.email=t@t commit -qm 0165

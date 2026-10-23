#!/bin/bash
# 0174: Adreno 840 bring-up level 12, an indirect buffer (pandora_gpu=12).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0174.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0174-pandora-gpu-ib.patch
git -c user.name=t -c user.email=t@t commit -qm 0174

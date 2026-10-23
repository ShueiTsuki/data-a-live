#!/bin/bash
# 0166: Adreno 840 bring-up level 5, GMU cold start (pandora_gpu=5).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0166.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0166-pandora-gpu-gmu-start.patch
git -c user.name=t -c user.email=t@t commit -qm 0166

#!/bin/bash
# 0172: Adreno 840 bring-up level 10, the CP (pandora_gpu=10).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0172.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0172-pandora-gpu-cp.patch
git -c user.name=t -c user.email=t@t commit -qm 0172

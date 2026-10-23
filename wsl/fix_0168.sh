#!/bin/bash
# 0168: Adreno 840 bring-up level 7, HFI bandwidth table and START (pandora_gpu=7).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0168.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0168-pandora-gpu-hfi-start.patch
git -c user.name=t -c user.email=t@t commit -qm 0168

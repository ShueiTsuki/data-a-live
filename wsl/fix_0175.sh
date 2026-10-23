#!/bin/bash
# 0175: Adreno 840 bring-up level 13, buffer pool and framed submissions (pandora_gpu=13).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0175.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0175-pandora-gpu-submit.patch
git -c user.name=t -c user.email=t@t commit -qm 0175

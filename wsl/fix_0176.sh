#!/bin/bash
# 0176: Adreno 840 buffer pool reserved before launchd (pandora_gpu=13).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0176.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0176-pandora-gpu-pool-early.patch
git -c user.name=t -c user.email=t@t commit -qm 0176

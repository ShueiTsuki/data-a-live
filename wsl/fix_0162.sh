#!/bin/bash
# 0162: Adreno 840 bring-up level 1, read-only probe (pandora_gpu=1).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0162.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0162-pandora-gpu-probe.patch
git -c user.name=t -c user.email=t@t commit -qm 0162

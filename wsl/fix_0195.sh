#!/bin/bash
# 0195: no per-submission flush of every imported page (pandora_gpu_flushall=1 keeps it), submit timing.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0195.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0195-pandora-gpu-no-flushall.patch
git -c user.name=t -c user.email=t@t commit -qm 0195

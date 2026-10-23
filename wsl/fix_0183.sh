#!/bin/bash
# 0183: GPU bring-up without per-step log waits (pandora_gpu_slow=1 restores them).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0183.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0183-pandora-gpu-fast-steps.patch
git -c user.name=t -c user.email=t@t commit -qm 0183

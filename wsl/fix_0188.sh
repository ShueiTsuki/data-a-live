#!/bin/bash
# 0188: framebuffer copy invalidates GPU-written rows (pandora_gpu_metal=1).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0188.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0188-pandora-fb-gpu-coherent.patch
git -c user.name=t -c user.email=t@t commit -qm 0188

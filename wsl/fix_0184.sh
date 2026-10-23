#!/bin/bash
# 0184: AGXAccelerator (IOAcceleratorES) for Metal discovery, with pandora_gpu_metal=1.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0184.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0184-pandora-gpu-metal-accelerator.patch
git -c user.name=t -c user.email=t@t commit -qm 0184

#!/bin/bash
# 0178: Adreno 840 GPU buffer pool carved off by the loader (GPUPOOL=MiB).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0178.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0178-pandora-gpu-pool-carve.patch
git -c user.name=t -c user.email=t@t commit -qm 0178

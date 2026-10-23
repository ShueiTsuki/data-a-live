#!/bin/bash
# 0181: Adreno 840 level 15, the rest of a8xx hw_init (pandora_gpu=15).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0181.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0181-pandora-gpu-hw-init-rest.patch
git -c user.name=t -c user.email=t@t commit -qm 0181

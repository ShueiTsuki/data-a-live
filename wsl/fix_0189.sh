#!/bin/bash
# 0189: GPU watch log (per-client counts, ring, SMMU faults).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0189.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0189-pandora-gpu-watch.patch
git -c user.name=t -c user.email=t@t commit -qm 0189

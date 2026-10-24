#!/bin/bash
# 0198: GPU frequency governor after Linux msm_devfreq and KGSL (no swinging).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0198.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0198-pandora-gpu-dcvs2.patch
git -c user.name=t -c user.email=t@t commit -qm 0198

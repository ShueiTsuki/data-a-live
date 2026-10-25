#!/bin/bash
# 0229: the ADSP images from the firmware partition.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0229.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0229-adsp-firmware-partition.patch
git -c user.name=t -c user.email=t@t commit -qm 0229

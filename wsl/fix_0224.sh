#!/bin/bash
# 0224: GPU SMMU TLB maintenance as Linux arm-smmu; autoswipe for unattended tests.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0224.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0224-smmu-tlb-autoswipe.patch
git -c user.name=t -c user.email=t@t commit -qm 0224

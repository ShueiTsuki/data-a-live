#!/bin/bash
# 0148: sparse prelink index (UniXNU NativePrelink) + ApplePMGRNub for a prelinked original AMFI.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0148.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0148-pandora-prelink-pmgr-nub.patch
git -c user.name=t -c user.email=t@t commit -qm 0148

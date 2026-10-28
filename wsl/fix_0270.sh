#!/bin/bash
# 0270: the amps' Halo core started with Android's core control values.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0270.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0270-amp-core-control-0x81.patch
git -c user.name=t -c user.email=t@t commit -qm 0270

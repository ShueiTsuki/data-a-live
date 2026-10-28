#!/bin/bash
# 0280: top amp receiver switch GPIO 174 high.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0280.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0280-top-amp-rcv-switch.patch
git -c user.name=t -c user.email=t@t commit -qm 0280

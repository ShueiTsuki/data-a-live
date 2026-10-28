#!/bin/bash
# 0265: the amps' PLL as Android sets it, the Halo core restarted with the clock.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0265.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0265-amp-pll-halo-restart.patch
git -c user.name=t -c user.email=t@t commit -qm 0265

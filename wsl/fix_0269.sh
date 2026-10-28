#!/bin/bash
# 0269: the amps loaded and started on a locked PLL, in Android's order.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0269.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0269-amp-load-on-locked-pll.patch
git -c user.name=t -c user.email=t@t commit -qm 0269

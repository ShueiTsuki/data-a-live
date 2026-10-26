#!/bin/bash
# 0238: the amplifiers DSP ROM stopped before the load.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0238.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0238-amp-rom-stop.patch
git -c user.name=t -c user.email=t@t commit -qm 0238

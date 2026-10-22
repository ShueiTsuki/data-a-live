#!/bin/bash
# 0155: IOAESAccelerator and a /defaults nub for the original AppleImage4 start.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0155.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0155-pandora-aes-defaults.patch
git -c user.name=t -c user.email=t@t commit -qm 0155

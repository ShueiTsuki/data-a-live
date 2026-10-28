#!/bin/bash
# 0251: USB module windows from kernel_map.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0251.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0251-usb-map-kernel-map.patch
git -c user.name=t -c user.email=t@t commit -qm 0251

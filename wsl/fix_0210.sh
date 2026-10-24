#!/bin/bash
# 0210: IORTC published (no 30 s wait in IOKitInitializeTime); native-4K signature check timed.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0210.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0210-pandora-iortc-cs4k-timing.patch
git -c user.name=t -c user.email=t@t commit -qm 0210

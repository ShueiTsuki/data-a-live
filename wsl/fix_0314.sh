#!/bin/bash
# 0314: FairPlay's kernel driver by Apple's class names, answering every call "unsupported".
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0314.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0314-fairplay-unsupported.patch
git -c user.name=t -c user.email=t@t commit -qm 0314

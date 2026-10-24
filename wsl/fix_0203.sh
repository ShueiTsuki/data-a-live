#!/bin/bash
# 0203: Big cores first (pandora_bigfirst); domain 0 fast channel, domain 1 floor.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0203.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0203-pandora-bigfirst.patch
git -c user.name=t -c user.email=t@t commit -qm 0203

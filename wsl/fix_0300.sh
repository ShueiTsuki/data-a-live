#!/bin/bash
# 0300: the receiver; graphs and amps follow the route.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0300.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0300-audio-receiver-route.patch
git -c user.name=t -c user.email=t@t commit -qm 0300

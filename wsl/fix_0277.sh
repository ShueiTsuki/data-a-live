#!/bin/bash
# 0277: a 15 s steady tone at -12 dBFS, readouts every second.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0277.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0277-steady-tone-15s.patch
git -c user.name=t -c user.email=t@t commit -qm 0277

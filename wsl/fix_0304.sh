#!/bin/bash
# 0304: the receiver TDM interface.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0304.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0304-audio-receiver-tdm.patch
git -c user.name=t -c user.email=t@t commit -qm 0304

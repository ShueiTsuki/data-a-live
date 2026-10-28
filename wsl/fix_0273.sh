#!/bin/bash
# 0273: the tone's packet buffer holds the 24-byte GPR header.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0273.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0273-tone-packet-size.patch
git -c user.name=t -c user.email=t@t commit -qm 0273

#!/bin/bash
# 0152: SWEET AMFI steps aside whenever original kexts are prelinked (decided by __PRELINK_TEXT).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0152.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0152-pandora-sweet-amfi-prelink-check.patch
git -c user.name=t -c user.email=t@t commit -qm 0152

#!/bin/bash
# 0297: installer removes (! - PATH).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0297.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0297-installer-remove.patch
git -c user.name=t -c user.email=t@t commit -qm 0297

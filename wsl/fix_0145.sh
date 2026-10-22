#!/bin/bash
# 0145: ADSP: shut down lite (0x1f/0x29) and our pas ids before loading, keep going after a DTB failure, signed error codes.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0145.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0145-pandora-adsp-shutdown-first.patch
git -c user.name=t -c user.email=t@t commit -qm 0145

#!/bin/bash
# 0186: GPU user client answers GetStatus.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0186.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0186-pandora-gpu-status.patch
git -c user.name=t -c user.email=t@t commit -qm 0186

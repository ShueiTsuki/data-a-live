#!/bin/bash
# 0187: GPU BoImport, buffers over the caller's pinned pages.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0187.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0187-pandora-gpu-bo-import.patch
git -c user.name=t -c user.email=t@t commit -qm 0187

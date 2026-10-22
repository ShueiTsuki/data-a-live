#!/bin/bash
# 0147: file installer: files.txt in the pinned pack -> root fs at boot (PNG wallpaper); pack directory up to 63 files.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0147.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0147-pandora-file-installer.patch
git -c user.name=t -c user.email=t@t commit -qm 0147

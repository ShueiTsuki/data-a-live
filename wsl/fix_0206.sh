#!/bin/bash
# 0206: no boot text on screen without -v (the loader's boot logo stays until the first frame).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0206.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0206-pandora-boot-logo-quiet.patch
git -c user.name=t -c user.email=t@t commit -qm 0206

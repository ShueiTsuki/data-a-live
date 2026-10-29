#!/bin/bash
# 0315: CONFIG_JETSAM in the PANDORA kernel configuration (needs a CI build: it changes core structures).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0315.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0315-jetsam.patch
git -c user.name=t -c user.email=t@t commit -qm 0315

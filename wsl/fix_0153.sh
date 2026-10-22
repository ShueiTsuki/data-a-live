#!/bin/bash
# 0153: start the ApplePMGRNub publisher before the security extensions load (AMFI waits only 30 s).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0153.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0153-pandora-pmgr-nub-early.patch
git -c user.name=t -c user.email=t@t commit -qm 0153

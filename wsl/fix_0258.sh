#!/bin/bash
# 0258: ring readers see only finished bytes.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0258.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0258-log-ring-done.patch
git -c user.name=t -c user.email=t@t commit -qm 0258

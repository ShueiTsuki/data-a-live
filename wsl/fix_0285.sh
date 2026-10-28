#!/bin/bash
# 0285: pandora_pathlog (plug-in paths a process looks at).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0285.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0285-pathlog.patch
git -c user.name=t -c user.email=t@t commit -qm 0285

#!/bin/bash
# 0272: the test tone with DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0272.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0272-tone-v2-data-command.patch
git -c user.name=t -c user.email=t@t commit -qm 0272

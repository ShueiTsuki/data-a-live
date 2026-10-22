#!/bin/bash
# 0159: log layer-set changes; pandora_fbdump frame dumps into the log file.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0159.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0159-pandora-layers-fbdump.patch
git -c user.name=t -c user.email=t@t commit -qm 0159

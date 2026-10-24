#!/bin/bash
# 0201: CPU governor over SCMI (pandora_cpu=2).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0201.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0201-pandora-cpu-governor.patch
git -c user.name=t -c user.email=t@t commit -qm 0201

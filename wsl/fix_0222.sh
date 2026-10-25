#!/bin/bash
# 0222: a sampling profiler for one process (pandora_prof=NAME).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0222.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0222-pandora-prof.patch
git -c user.name=t -c user.email=t@t commit -qm 0222

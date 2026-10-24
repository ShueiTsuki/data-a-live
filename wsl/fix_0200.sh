#!/bin/bash
# 0200: CPU performance probe (clusters, clocks, SCMI performance protocol), read only.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0200.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0200-pandora-cpu-probe.patch
git -c user.name=t -c user.email=t@t commit -qm 0200

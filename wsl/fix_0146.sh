#!/bin/bash
# 0146: ADSP: no SHM bridge of our own for the PIL metadata (the hypervisor makes it).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0146.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0146-pandora-adsp-no-own-bridge.patch
git -c user.name=t -c user.email=t@t commit -qm 0146

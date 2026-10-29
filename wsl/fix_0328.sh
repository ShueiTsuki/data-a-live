#!/bin/bash
# 0328: the context entry on every change of submitting client.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0328.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0328-gpu-context-switch.patch
git -c user.name=t -c user.email=t@t commit -qm 0328

#!/bin/bash
# 0329: sandbox extensions issued and consumed, container paths answered.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0329.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0329-sandbox-extensions.patch
git -c user.name=t -c user.email=t@t commit -qm 0329

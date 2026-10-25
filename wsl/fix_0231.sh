#!/bin/bash
# 0231: the SOCCP GLINK session and ping.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0231.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0231-soccp-glink-session.patch
git -c user.name=t -c user.email=t@t commit -qm 0231

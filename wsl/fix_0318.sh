#!/bin/bash
# 0318: BoFree waits for the client's own submissions only.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0318.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0318-gpu-free-own-fence.patch
git -c user.name=t -c user.email=t@t commit -qm 0318

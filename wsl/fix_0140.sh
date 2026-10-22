#!/bin/bash
# 0140: DSI: shared FIFO send, read the panel brightness back with DCS 0x52 after setting it;
# the read-back shows whether the panel took the level.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0140.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0140-pandora-dsi-brightness-readback.patch
git -c user.name=t -c user.email=t@t commit -qm 0140

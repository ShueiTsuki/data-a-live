#!/bin/bash
# 0141: DSI read-back again 100 ms after the brightness write (the immediate read showed the old level).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0141.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0141-pandora-dsi-readback-delayed.patch
git -c user.name=t -c user.email=t@t commit -qm 0141

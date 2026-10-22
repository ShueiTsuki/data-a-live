#!/bin/bash
# 0139: panel brightness by DCS 0x51 through the DSI0 test-pattern FIFO (SM8850 msm_drm offsets);
# pixel scaling stays as the fallback (pandora_dsi_bl=0 or a command timeout).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0139.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0139-pandora-dsi-brightness.patch
git -c user.name=t -c user.email=t@t commit -qm 0139

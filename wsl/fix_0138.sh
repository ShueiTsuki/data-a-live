#!/bin/bash
# 0138: backlight: only "brightness" (0..65536) sets the pixel scale, 256*sqrt, floor 32;
# brightness-nits=0 from CoreBrightness had made the screen nearly black.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0138.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0138-pandora-backlight-brightness-only.patch
git -c user.name=t -c user.email=t@t commit -qm 0138

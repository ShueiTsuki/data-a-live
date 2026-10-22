#!/bin/bash
# 0137: AppleARMBacklight service (backlight-control, IODisplayParameters) for CoreBrightness;
# the brightness it sets scales the pixels (0136), every property written is logged.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0137.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0137-pandora-backlight-service.patch
git -c user.name=t -c user.email=t@t commit -qm 0137

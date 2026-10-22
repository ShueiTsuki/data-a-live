#!/bin/bash
# 0136: PANDORA screen brightness from iOS. IOMobileFramebuffer's kern_SwapSetBrightness
# stores a double and a flag in the swap record SwapEnd sends (record = object + 0x18:
# flag 0x2e7, value 0x2ec; brightness limit flag 0x2e8, value 0x2f4; disassembly of the
# 16.7.16 dyld cache). The panel's own brightness stays as ABL set it; the CPU copy scales
# each pixel instead (AMOLED: darker pixels are dimmer). Values and the selector-50
# (SetBrightnessCorrection) scalars are logged so the unit can be checked.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0136.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0136-pandora-software-brightness.patch
git -c user.name=t -c user.email=t@t commit -qm 0136

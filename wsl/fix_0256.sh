#!/bin/bash
# 0256: thermistor GPIOs in high impedance.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0256.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0256-therm-gpio-highz.patch
git -c user.name=t -c user.email=t@t commit -qm 0256

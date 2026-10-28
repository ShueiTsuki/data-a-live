#!/bin/bash
# 0263: brightness (iOS sensors opt-in, logo level restored, panel dimming and FadePeriod).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0263.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0263-brightness.patch
git -c user.name=t -c user.email=t@t commit -qm 0263

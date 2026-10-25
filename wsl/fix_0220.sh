#!/bin/bash
# 0220: 120 Hz for CoreAnimation (IOMFBDisplayRefresh), pacing numbers, touch count, interrupt and PSCI probes.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0220.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0220-pandora-120hz-probes.patch
git -c user.name=t -c user.email=t@t commit -qm 0220

#!/bin/bash
# 0268: firmware read back whole; reload after a reset with the clock, two ways of writing.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0268.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0268-amp-verify-reload.patch
git -c user.name=t -c user.email=t@t commit -qm 0268

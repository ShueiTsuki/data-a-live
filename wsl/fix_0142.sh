#!/bin/bash
# 0142: input self-test (pandora_selftest=SECONDS: kernel presses keys, swipes, logs screen thumbnails).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0142.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0142-pandora-input-selftest.patch
git -c user.name=t -c user.email=t@t commit -qm 0142

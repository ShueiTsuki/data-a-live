#!/bin/bash
# 0303: the Codec microphone stream offers mono and two channels.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0303.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0303-audio-mic-formats.patch
git -c user.name=t -c user.email=t@t commit -qm 0303

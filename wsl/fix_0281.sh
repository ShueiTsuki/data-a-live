#!/bin/bash
# 0281: IOAudio2Device step 1 (no sound).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0281.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0281-ioaudio2-device-step1.patch
git -c user.name=t -c user.email=t@t commit -qm 0281

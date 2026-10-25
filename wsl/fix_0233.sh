#!/bin/bash
# 0233: the SMMU survey for the audio streams.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0233.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0233-smmu-audio-survey.patch
git -c user.name=t -c user.email=t@t commit -qm 0233

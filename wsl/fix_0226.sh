#!/bin/bash
# 0226: talking to the audio DSP (SMP2P ready/handover, GLINK, GPR channel, APM question).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0226.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0226-adsp-glink-gpr.patch
git -c user.name=t -c user.email=t@t commit -qm 0226

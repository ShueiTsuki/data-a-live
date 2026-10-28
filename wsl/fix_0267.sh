#!/bin/bash
# 0267: violation addresses; one amp loaded again with the clock running.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0267.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0267-amp-reload-with-clock.patch
git -c user.name=t -c user.email=t@t commit -qm 0267

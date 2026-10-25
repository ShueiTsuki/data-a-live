#!/bin/bash
# 0218: IOMFB's swap queue and SwapWait (iOS 16 swap_wait_gated); frames go to the display from their own thread.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0218.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0218-pandora-swap-queue.patch
git -c user.name=t -c user.email=t@t commit -qm 0218

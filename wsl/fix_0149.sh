#!/bin/bash
# 0149: keep the kernel PRNG of a prelinked corecrypto kext (SWEET built-in only without it).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0149.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0149-pandora-prng-prelinked-corecrypto.patch
git -c user.name=t -c user.email=t@t commit -qm 0149

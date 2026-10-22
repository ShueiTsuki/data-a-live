#!/bin/bash
# 0150: iOS vtable layout (no reserved padding slots) so prelinked original iOS kexts call the right methods.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0150.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0150-pandora-ios-vtable-layout.patch
git -c user.name=t -c user.email=t@t commit -qm 0150

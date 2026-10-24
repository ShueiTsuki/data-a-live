#!/bin/bash
# 0209: inline asm one instruction per statement (";" is a comment in the Mach-O assembler): the log time stamps read the counter.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0209.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0209-pandora-asm-separators.patch
git -c user.name=t -c user.email=t@t commit -qm 0209

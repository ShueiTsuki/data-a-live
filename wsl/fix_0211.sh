#!/bin/bash
# 0211: SHA-256 with the ARMv8 SHA-2 instructions for SHA256_Init/Update/Final (code-signing hashes at boot).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0211.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0211-pandora-sha256-ce.patch
git -c user.name=t -c user.email=t@t commit -qm 0211

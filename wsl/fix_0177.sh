#!/bin/bash
# 0177: Adreno 840 level 14, buffer objects, submissions and the NativeAdreno user client (pandora_gpu=14).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0177.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0177-pandora-gpu-user-client.patch
git -c user.name=t -c user.email=t@t commit -qm 0177

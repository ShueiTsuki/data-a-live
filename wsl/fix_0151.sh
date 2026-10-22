#!/bin/bash
# 0151: early security initializer (UniXNU NativePrelink) and SWEET AMFI stepping aside for a prelinked original AMFI.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0151.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0151-pandora-original-amfi-early-init.patch
git -c user.name=t -c user.email=t@t commit -qm 0151

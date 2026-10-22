#!/bin/bash
# 0135: main screen touch on by default (pandora_touch=0xf: firmware reports + THP fallback).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
W=/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl
tr -d '\r' < $W/pandora_touch.c > bsd/dev/pandora_touch.c
git add -A
git diff --cached --stat
git diff --cached > $W/../xnu/patches/0135-pandora-touch-default-on.patch
git -c user.name=t -c user.email=t@t commit -qm 0135

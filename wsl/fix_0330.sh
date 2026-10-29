#!/bin/bash
# 0330: a daemon the kernel no longer starts runs as pandora_sink.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0330.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0330-crash-stop-sink.patch
git -c user.name=t -c user.email=t@t commit -qm 0330

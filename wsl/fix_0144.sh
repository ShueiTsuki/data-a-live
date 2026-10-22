#!/bin/bash
# 0144: audio DSP step 2: firmware pack file (pandora_fw_read), QMP load_state, LCX/LMX votes, PAS load + auth (pandora_adsp=2).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0144.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0144-pandora-adsp-load.patch
git -c user.name=t -c user.email=t@t commit -qm 0144

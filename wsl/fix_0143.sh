#!/bin/bash
# 0143: audio DSP step 1: SCM (TrustZone) query calls from XNU, boot-arg pandora_adsp=1.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 /mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/fix_0143.py
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0143-pandora-adsp-scm-probe.patch
git -c user.name=t -c user.email=t@t commit -qm 0143

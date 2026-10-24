#!/bin/bash
set -e
cd /mnt/d/DeepSeekHarness/claude/ios_pandora
git add -A
git status --short | grep -v -E '^(A |M ) *(xnu/patches/|wsl/|\.github/)' || true
git commit -q -m "PANDORA machine config: fuxi patches 0082-0101, 4 KiB pages, UniXNU native-4k patches

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git push -q origin main && git log --oneline -1
gh workflow run xnu.yml -R ShueiTsuki/data-a-live -f machine_config=PANDORA
sleep 8; gh run list -R ShueiTsuki/data-a-live -L 1

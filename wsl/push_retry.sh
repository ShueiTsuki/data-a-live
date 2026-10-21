#!/bin/bash
set -e
pkill -f 'git push' 2>/dev/null || true
git config --global --unset url."https://ghfast.top/https://github.com/".insteadOf || true
git config --global --get-regexp '^url\.' || echo "no global url rewrites"
cd /mnt/d/DeepSeekHarness/claude/ios_pandora
timeout 120 git push -q origin main && git log --oneline -1
gh api repos/ShueiTsuki/ios-pandora/commits/main --jq .sha | cut -c1-7
gh workflow run xnu.yml -R ShueiTsuki/ios-pandora -f machine_config=PANDORA
sleep 8; gh run list -R ShueiTsuki/ios-pandora -L 1

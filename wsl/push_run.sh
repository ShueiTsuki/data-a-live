#!/bin/bash
# Commit everything staged-able, push, start a PANDORA build. Usage: push_run.sh "commit message"
set -e
cd /mnt/d/DeepSeekHarness/claude/ios_pandora
git add -A
git status --short | grep -v -E '^(A |M ) *(xnu/patches/|wsl/|\.github/)' || true
git commit -q -m "$1

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
timeout 120 git push -q origin main && git log --oneline -1
gh workflow run xnu.yml -R ShueiTsuki/ios-pandora -f machine_config=PANDORA
sleep 10; gh run list -R ShueiTsuki/ios-pandora -L 1 --json databaseId,status --jq '.[0]|"\(.databaseId) \(.status)"'

#!/bin/bash
set -e
cd /mnt/d/DeepSeekHarness/claude/ios_pandora
git add -A
git status --short | grep -v -E '^(A |M ) *(xnu/|\.github/|wsl/)' || true
git commit -q -m "Add ios-sweet XNU patch series and manual-only macOS build workflow

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git push -q origin main
git log --oneline -1
gh workflow run xnu.yml -R ShueiTsuki/ios-pandora
sleep 8
gh run list -R ShueiTsuki/ios-pandora -L 1

#!/bin/bash
set -e
cd /mnt/d/DeepSeekHarness/claude/ios_pandora
git config --global --add safe.directory /mnt/d/DeepSeekHarness/claude/ios_pandora
gh auth setup-git
id=$(gh api user --jq .id); login=$(gh api user --jq .login)
git config user.name "$login"
git config user.email "$id+$login@users.noreply.github.com"
git add -A
git commit -q -m "Initial import: plan, hardware notes, step-1 hello loader and tools

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git log --oneline -1
gh repo create "$login/ios-pandora" --private --source . --remote origin --push
gh repo view "$login/ios-pandora" --json visibility,url

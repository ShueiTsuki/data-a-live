#!/bin/bash
# 0132: keys on by default on PANDORA (power, volume down over SPMI v8, volume up on TLMM
# GPIO 101, all read stably in run 29's test); fuxikeys=0 turns them off.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'bsd/dev/fuxi_keys.c'
t = open(p).read()
old = """#if defined(PANDORA)
	/* Xiaomi 13 (SM8550) register addresses; the 17 Pro's differ. Off unless fuxikeys=1. */
	int enabled = 0;
#else"""
new = """#if defined(PANDORA)
	/* 17 Pro: pk_setup() (SPMI arbiter v8, TLMM GPIO 101). Off with fuxikeys=0. */
	int enabled = 1;
#else"""
assert t.count(old) == 1
t = t.replace(old, new)
open(p, 'w').write(t)
print('patched')
PY
git add -A
git diff --cached --stat
git diff --cached > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0132-pandora-keys-default-on.patch
git -c user.name=t -c user.email=t@t commit -qm 0132

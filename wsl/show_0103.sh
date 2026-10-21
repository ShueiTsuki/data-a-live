#!/bin/bash
P=/mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0103-unixnu-native-4k-bootstrap-capacity.patch
sed -n '/^diff --git a\/osfmk\/arm64\/start.s/,/^diff --git a\/osfmk\/arm64\/genassym/p' $P
cd /opt/work/tree
git apply --check --include=osfmk/arm/pmap/pmap.h --include=osfmk/arm64/genassym.c $P && echo other-files-ok
grep -n -E 'bzero|BOOTSTRAP_TABLE|bootstrap_pagetables|ARM_PGBYTES \* 4|4 \*' osfmk/arm64/start.s | head -20

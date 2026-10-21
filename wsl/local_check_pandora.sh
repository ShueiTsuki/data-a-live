#!/bin/bash
# Syntax-check files against the patched PANDORA tree, borrowing CI run 1's (SWEET) build dirs.
set -e
R=/opt/work/run1; B=/opt/work/bt-pandora
rm -rf $B && mkdir -p $B && cp -r $R/bt/build $R/bt/fakeroot $R/bt/sdk-kernel-platform $B/ && cp -r /opt/work/tree $B/xnu
cd $R
XNU_CC_EXTRA="-DFUXI=1 -DPANDORA=1" python3 /opt/work/tools/xnu-cc.py run1.full.log $B "$@" --per-file 2>&1 | grep -v '^ *[0-9]* <instantiation>' | tail -40

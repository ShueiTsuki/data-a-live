#!/bin/bash
# Syntax-check files against the patched tree, using CI run 2's PANDORA (4 KiB) build dirs and log.
set -e
R=/opt/work/run2; B=/opt/work/bt-pandora
if [ ! -d $R/bt ]; then mkdir -p $R/bt && tar xzf $R/xnu-buildtree/buildtree.tgz -C $R/bt; fi
rm -rf $B && mkdir -p $B && cp -r $R/bt/build $R/bt/fakeroot $R/bt/sdk-kernel-platform $B/ && cp -r /opt/work/tree $B/xnu
cd $R
XNU_CC_EXTRA="${EXTRA:-}" python3 /opt/work/tools/xnu-cc.py run2.full.log $B "$@" --per-file 2>&1 | grep -v '^ *[0-9]* <instantiation>' | tail -40

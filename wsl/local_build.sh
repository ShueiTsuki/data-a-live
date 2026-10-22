#!/bin/bash
# local_build.sh CIRUN OUT SRC...: kernel from CI run CIRUN's objects (/opt/work/runCIRUN, see
# local_relink.py --ci) with the listed sources (paths in /opt/work/tree, e.g.
# bsd/kern/sweet_amfi.c) recompiled locally from the current tree. Writes OUT/kernel.development.pandora.
# Locally compiled objects lack Apple clang's kalloc_type builtins (xnu-cc.py stubs them):
# fine for quick tests; build release kernels in CI.
set -euo pipefail
R=/opt/work/run$1; OUT=$2; shift 2
LOG=/opt/work/run48/run48.full.log
B=$R/bt-local
rm -rf $B && mkdir -p $B && cp -r $R/bt/build $R/bt/fakeroot $R/bt/sdk-kernel-platform $B/ && cp -r /opt/work/tree $B/xnu
O=$OUT/objs; rm -rf $OUT && mkdir -p $O
XNU_CC_OBJDIR=$O python3 /opt/work/tools/xnu-cc.py $LOG $B "$@" 2>&1 | tail -2
local=(); for s in "$@"; do local+=(--local "$s"); done
python3 /opt/work/sh/local_relink.py $LOG $B $O $OUT --ci $R/ci "${local[@]}" 2>&1 | grep -v '^/opt/work/ld64\|Frameworks' | tail -4
/opt/work/ld64/inst/bin/aarch64-apple-darwin-strip -S $OUT/kernel.development.pandora.unstripped -o $OUT/kernel.development.pandora
sha256sum $OUT/kernel.development.pandora

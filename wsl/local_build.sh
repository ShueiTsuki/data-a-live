#!/bin/bash
# local_build.sh CIRUN OUT SRC...: kernel from CI run CIRUN's objects (/opt/work/runCIRUN, see
# local_relink.py --ci; wsl/ci_fetch.sh lays a run out there) with the listed sources (paths in
# /opt/work/tree, e.g. bsd/kern/sweet_amfi.c, new files too) recompiled locally from the current tree.
# Writes OUT/kernel.development.pandora.
# Locally compiled objects lack Apple clang's kalloc_type builtins (xnu-cc.py stubs them): our own
# drivers are fine, core xnu files that allocate with kalloc_type/kfree_type are not (they panic);
# changes to those, and kernel configuration changes, go through CI.
set -euo pipefail
R=/opt/work/run$1; OUT=$2; shift 2
# CI log of an xnu.yml run (compile and link command lines); kept outside the runN dirs. The run's own
# log when ci_fetch.sh got it, else run 54's (the commands of runs 52-54 are the same).
LOG=/opt/work/cilog/xnu-ci-$(basename $R | sed 's/^run//').full.log
[ -f $LOG ] || LOG=/opt/work/cilog/xnu-ci-54.full.log
# The runner's checkout path in that log's commands (the repository was renamed ios-pandora -> data-a-live).
XNU_CI_DXB=$(grep -a -o -m1 '/Users/runner/work/[^/ ]*/[^/ ]*/dxb' $LOG || true)
export XNU_CI_DXB=${XNU_CI_DXB:-/Users/runner/work/ios-pandora/ios-pandora/dxb}
echo "CI run $(basename $R), log $(basename $LOG), dxb $XNU_CI_DXB"
B=$R/bt-local
# The run's build dirs are only read: copied once; the source tree is synced from /opt/work/tree each time.
if [ ! -d $B/build ]; then
	rm -rf $B && mkdir -p $B && cp -r $R/bt/build $R/bt/fakeroot $R/bt/sdk-kernel-platform $B/
fi
mkdir -p $B/xnu && rsync -a --delete --exclude .git /opt/work/tree/ $B/xnu/
O=$OUT/objs; rm -rf $OUT && mkdir -p $O
XNU_CC_OBJDIR=$O python3 /opt/work/tools/xnu-cc.py $LOG $B "$@" 2>&1 | tail -2
local=(); for s in "$@"; do local+=(--local "$s"); done
python3 /opt/work/sh/local_relink.py $LOG $B $O $OUT --ci $R/ci "${local[@]}" 2>&1 | grep -v '^/opt/work/ld64\|Frameworks' | tail -4
/opt/work/ld64/inst/bin/aarch64-apple-darwin-strip -S $OUT/kernel.development.pandora.unstripped -o $OUT/kernel.development.pandora
sha256sum $OUT/kernel.development.pandora

#!/bin/bash
set -e
R=/opt/work/run1; O=$R/objs; mkdir -p $O
cd $R
XNU_CC_OBJDIR=$O python3 /opt/work/tools/xnu-cc.py run1.full.log bt "$@" 2>&1 | tail -6
ls $O | wc -l
f=$(ls $O | head -1); file $O/$f
/opt/swift/usr/bin/llvm-nm -u $O/osfmk__arm64__sweet_early.c.o 2>/dev/null | head -5

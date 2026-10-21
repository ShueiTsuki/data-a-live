#!/bin/bash
set -e
D=/opt/work/dxb
[ -d $D/.git ] || { git init -q $D; git -C $D fetch -q --depth 1 https://github.com/blacktop/darwin-xnu-build 663906528d1cd5ea8fde7de6f16de73db9511549; git -C $D checkout -q FETCH_HEAD; }
ls $D/patches | head -30
grep -n -E 'patches|git apply|patch -p' $D/build.sh | head -30

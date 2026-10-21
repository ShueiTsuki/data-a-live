#!/bin/bash
# Re-run compile commands from CI run 1 locally (ios-sweet's xnu-cc.py, paths adapted).
set -e
W=/opt/work; R=$W/run1
mkdir -p $W/tools
sed -e "s#/Users/runner/work/ios-sweet/ios-sweet/dxb#/Users/runner/work/ios-pandora/ios-pandora/dxb#" \
    -e "s#os.path.expanduser('~/swift/usr/bin/clang')#'/opt/swift/usr/bin/clang'#" \
    /mnt/d/DeepSeekHarness/claude/ios_pandora/ref/ios-sweet/tools/xnu-cc.py > $W/tools/xnu-cc.py
grep -n -E "^CI_DXB|^CLANG" $W/tools/xnu-cc.py
grep -c '/Users/runner/work/ios-pandora/ios-pandora/dxb/xnu/' $R/run1.full.log
cd $R
python3 $W/tools/xnu-cc.py run1.full.log bt "$@" --per-file 2>&1 | tail -25

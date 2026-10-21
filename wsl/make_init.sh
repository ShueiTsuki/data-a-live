#!/bin/bash
# Build ios-sweet's sweet-init (PID 1 test program) as a static iOS Mach-O: /opt/work/init/sweet-init
set -e
S=/mnt/d/DeepSeekHarness/claude/ios_pandora/ref/ios-sweet/tools; O=/opt/work/init; mkdir -p $O
tr -d '\r' < $S/sweet-init.S > $O/sweet-init.S
tr -d '\r' < $S/mkinit.py > /opt/work/tools/mkinit.py
/opt/swift/usr/bin/clang -target arm64-apple-ios16.0 -c $O/sweet-init.S -o $O/sweet-init.o
python3 /opt/work/tools/mkinit.py $O/sweet-init.o $O/sweet-init
ls -l $O/sweet-init; file $O/sweet-init

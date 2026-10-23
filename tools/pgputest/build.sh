#!/bin/bash
# Build pgputest for iOS 16 (arm64) without an SDK, ad-hoc signed by ld64, and its trust cache.
#   tools/pgputest/build.sh [OUTDIR]     (in WSL; default /opt/work/pgputest)
# Output: OUTDIR/pgputest and OUTDIR/pgputest.tc (merge with '+' into TRUSTCACHE's second module).
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
O=${1:-/opt/work/pgputest}
CC="/opt/swift/usr/bin/clang -target arm64-apple-ios16.0 -ffreestanding -fno-stack-protector -O2 -Wall"
LD="/opt/work/ld64/inst/bin/aarch64-apple-darwin-ld -arch arm64 -platform_version ios 16.0 16.0"
mkdir -p "$O"
# Stub libraries with the real install names (stubs.c).
$CC -w -DSTUB_LIBSYSTEM -c "$D/stubs.c" -o "$O/stub_system.o"
$CC -w -c "$D/stubs.c" -o "$O/stub_iokit.o"
$LD -dylib -install_name /usr/lib/libSystem.B.dylib -o "$O/libSystem.B.dylib" "$O/stub_system.o"
$LD -dylib -install_name /System/Library/Frameworks/IOKit.framework/Versions/A/IOKit -o "$O/IOKit" "$O/stub_iokit.o"
$CC -Wextra -c "$D/pgputest.c" -o "$O/pgputest.o"
$LD -e _main -adhoc_codesign -o "$O/pgputest" "$O/pgputest.o" "$O/libSystem.B.dylib" "$O/IOKit"
python3 -I "$D/mktc.py" "$O/pgputest.tc" "$O/pgputest"
ls -l "$O/pgputest" "$O/pgputest.tc"

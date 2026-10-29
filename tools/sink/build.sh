#!/bin/bash
# Build pandora_sink (tools/sink/pandora_sink.c) for iOS 16 arm64 in WSL with the local SDK, ad-hoc signed.
#   tools/sink/build.sh [OUTDIR]     (default /opt/work/sink)
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
O=${1:-/opt/work/sink}
SDK=/opt/work/xcode/sdk16/iPhoneOS16.1.sdk
mkdir -p "$O"
clang-18 -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -Wall \
	-fuse-ld=/usr/lib/llvm-18/bin/ld64.lld -mlinker-version=907 \
	-framework CoreFoundation \
	-o "$O/pandora_sink" "$D/pandora_sink.c"
/root/.cargo/bin/rcodesign sign --binary-identifier com.data-a-live.sink "$O/pandora_sink"
ls -l "$O/pandora_sink"

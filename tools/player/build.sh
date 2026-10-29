#!/bin/bash
# Build PandoraPlayer (tools/player/PandoraPlayer.m) for iOS 16 arm64 in WSL with the local SDK, ad-hoc signed.
#   tools/player/build.sh [OUTDIR]     (default /opt/work/player)
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
O=${1:-/opt/work/player}
SDK=/opt/work/xcode/sdk16/iPhoneOS16.1.sdk
mkdir -p "$O"
clang-18 -target arm64-apple-ios16.0 -isysroot "$SDK" -fobjc-arc -O2 -Wall \
	-fuse-ld=/usr/lib/llvm-18/bin/ld64.lld -mlinker-version=907 \
	-framework UIKit -framework AVFoundation -framework Foundation \
	-o "$O/PandoraPlayer" "$D/PandoraPlayer.m"
/root/.cargo/bin/rcodesign sign --binary-identifier com.apple.camera "$O/PandoraPlayer"
ls -l "$O/PandoraPlayer"

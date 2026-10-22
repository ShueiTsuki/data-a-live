#!/bin/bash
# Build pinfile (static arm64 Android binary) with the NDK.
set -euo pipefail
cd "$(dirname "$0")"
T="D:/Android/ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin"
"$T/clang.exe" --target=aarch64-linux-android31 -static -O2 -Wall -Wextra -Werror -o pinfile pinfile.c
sha256sum pinfile

#!/bin/bash
# Build pandora-hello with the NDK's LLVM tools: out/Image and out/hello-boot.img.
set -euo pipefail
cd "$(dirname "$0")"
T="D:/Android/ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin"
out=out
mkdir -p $out
flags=(--target=aarch64-none-elf -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic
       -fno-stack-protector -mgeneral-regs-only -mstrict-align -Wall -Wextra -Werror)
"$T/clang.exe" "${flags[@]}" -c start.S -o $out/start.o
"$T/clang.exe" "${flags[@]}" -c hello.c -o $out/hello.o
# Loaded at an unknown address: refuse absolute relocations in C.
if "$T/llvm-readelf.exe" -r $out/hello.o | grep -q R_AARCH64_ABS; then
	echo "absolute relocation in hello.c" >&2; exit 1
fi
"$T/ld.lld.exe" -T hello.ld --build-id=none $out/start.o $out/hello.o -o $out/hello.elf
"$T/llvm-objcopy.exe" -O binary $out/hello.elf $out/Image
python -I ../../tools/mkbootimg_v4.py $out/Image $out/hello-boot.img
python -I ../../tools/bootimg_info.py $out/hello-boot.img
sha256sum $out/hello-boot.img

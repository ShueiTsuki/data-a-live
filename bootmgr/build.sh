#!/bin/bash
# Build the boot menu for boot_b with the NDK's LLVM tools: out/boot-menu.img, a full
# partition image (menu + XNU payload + the stock Android kernel + the stock AVB vbmeta).
#
#   bootmgr/build.sh [--payload FILE] [--stock FILE] [--wait-ms N] [--any-mode] [--unarmed]
#
# --payload  XNU payload (default shim/loader/out/payload.bin, copied with everyday boot-args)
# --stock    the phone's stock boot_b, full partition copy (default device/stock/boot_b.img)
# --wait-ms  how long the line stays on the boot logo (default 3000)
# --any-mode test builds: the line in every boot mode (fastboot boot may not look "normal")
# --unarmed  volume up shows "iOS off" instead of starting iOS
# Writes nothing to the phone.
set -euo pipefail
cd "$(dirname "$0")"
T="D:/Android/ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin"
payload=../shim/loader/out/payload.bin
stock=../device/stock/boot_b.img
mk=()
while [ $# -gt 0 ]; do
	case $1 in
	--payload) payload=$2; shift 2 ;;
	--stock) stock=$2; shift 2 ;;
	--wait-ms) mk+=(--wait-ms "$2"); shift 2 ;;
	--any-mode|--unarmed) mk+=("$1"); shift ;;
	*) sed -n '2,13p' "$0"; exit 2 ;;
	esac
done
out=out
mkdir -p $out
python -I prep.py "$payload" "$stock" $out
flags=(--target=aarch64-none-elf -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic
       -fno-stack-protector -mgeneral-regs-only -mstrict-align -Wall -Wextra -Werror)
"$T/clang.exe" "${flags[@]}" -c start.S -o $out/start.o
"$T/clang.exe" "${flags[@]}" -DPAYLOAD=\"$out/payload.bin\" -DLINUX=\"$out/linux.Image\" -c blobs.S -o $out/blobs.o
"$T/clang.exe" "${flags[@]}" -c menu.c -o $out/menu.o
# Loaded at an unknown address: refuse absolute relocations in C.
if "$T/llvm-readelf.exe" -r $out/menu.o | grep -q R_AARCH64_ABS; then
	echo "absolute relocation in menu.c" >&2; exit 1
fi
"$T/ld.lld.exe" -T menu.ld --build-id=none $out/start.o $out/menu.o $out/blobs.o -o $out/menu.elf
# No library calls may have crept in (memcpy/memset from initialisers).
if "$T/llvm-nm.exe" -u $out/menu.elf | grep -q .; then
	echo "undefined symbols:" >&2; "$T/llvm-nm.exe" -u $out/menu.elf >&2; exit 1
fi
"$T/llvm-objcopy.exe" -O binary $out/menu.elf $out/Image
python -I mkboot.py $out "$stock" "${mk[@]}"

#!/bin/bash
# Build the boot menu for boot_b with the NDK's LLVM tools: out/boot-menu.img, a full
# partition image (menu + the stock Android kernel + the iOS slot + the stock AVB vbmeta).
#
#   bootmgr/build.sh [--slot-from LOADER] [--stock FILE] [--wait-ms N] [--any-mode]
#
# --slot-from  fill the iOS slot from LOADER (shim/loader/out/loader-boot.img or its Image),
#              checked against the newest saved extents in device/extents; without it the
#              slot is empty ("iOS off") until update_ios.sh fills it
# --stock      the phone's stock boot_b, full partition copy (default device/stock/boot_b.img)
# --wait-ms    how long the line stays on the boot logo (default 5000)
# --any-mode   test builds: the line in every boot mode (fastboot boot does not look "normal"),
#              plus the numbers the menu decides from
# Writes nothing to the phone.
set -euo pipefail
cd "$(dirname "$0")"
T="D:/Android/ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin"
stock=../device/stock/boot_b.img
loader=
mk=()
while [ $# -gt 0 ]; do
	case $1 in
	--slot-from) loader=$2; shift 2 ;;
	--stock) stock=$2; shift 2 ;;
	--wait-ms) mk+=(--wait-ms "$2"); shift 2 ;;
	--any-mode) mk+=("$1"); shift ;;
	*) sed -n '2,15p' "$0"; exit 2 ;;
	esac
done
out=out
mkdir -p $out
python -I prep.py "$stock" $out
if [ -n "$loader" ]; then
	newest() { ls ../device/extents/$1-*.json | sort | tail -1; }
	python -I slot.py "$loader" $out/slot.img --label "$(basename "$loader") $(date +%Y%m%d-%H%M%S)" \
		--extents "$(newest iosroot)" "$(newest ioslog)" "$(newest adspfw)"
	mk+=(--slot $out/slot.img)
fi
flags=(--target=aarch64-none-elf -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic
       -fno-stack-protector -mgeneral-regs-only -mstrict-align -Wall -Wextra -Werror)
"$T/clang.exe" "${flags[@]}" -c start.S -o $out/start.o
"$T/clang.exe" "${flags[@]}" -DLINUX=\"$out/linux.Image\" -c blobs.S -o $out/blobs.o
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

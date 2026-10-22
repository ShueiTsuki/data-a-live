#!/bin/bash
# qemu_phone.sh OUTDIR SECS [KERNEL_FOR_SYMBOLS]: boot shim/loader/outq/Image (phone image, QEMU GIC)
# with tools/qemu_loader.py. vm.overcommit_memory=1 only while QEMU runs (it maps 47 GiB).
set -u
P=/mnt/d/DeepSeekHarness/claude/ios_pandora
O=$1; SECS=${2:-120}; K=${3:-}
old=$(cat /proc/sys/vm/overcommit_memory)
trap 'echo $old > /proc/sys/vm/overcommit_memory' EXIT
echo 1 > /proc/sys/vm/overcommit_memory
tr -d '\r' < $P/tools/qemu_loader.py > /opt/work/sh/qemu_loader.py
python3 /opt/work/sh/qemu_loader.py $P/shim/loader/outq/Image $P/shim/loader/outq/payload.bin $O \
  --secs $SECS ${K:+--kernel $K} 2>&1 | tail -3
grep -v '^\s*$' $O/fb.txt | tail -60

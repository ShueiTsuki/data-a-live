#!/bin/bash
# Boot a PANDORA kernel in QEMU virt with ios-sweet's tools. Usage: qemu_run.sh RUNDIR SECS [extra cmdline]
set -e
R=$1; SECS=${2:-40}; EXTRA=${3:-}; RD=${RAMDISK:-}
T=/opt/work/tools; mkdir -p $T
for f in adt.py mkadt.py mkpayload.py qemu-sweet.py; do tr -d '\r' < /mnt/d/DeepSeekHarness/claude/ios_pandora/ref/ios-sweet/tools/$f > $T/$f; done
tr -d '\r' < /mnt/d/DeepSeekHarness/claude/ios_pandora/tools/mkadt_pandora.py > $T/mkadt_pandora.py
K=$R/xnu-kernel/kernel.development.pandora
U=$(find $R/xnu-kernel/kernel.development.pandora.link -type f -name '*.unstripped*' 2>/dev/null | head -1); [ -n "$U" ] || U=$K
echo "kernel $K, symbols $U"
O=$R/qemu; rm -rf $O; mkdir -p $O
python3 $T/mkadt_pandora.py $O/q.adt --qemu 2>&1 | tail -2
python3 $T/mkpayload.py $K $O/q.adt $O/payload.bin --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress $EXTRA" ${RD:+--ramdisk $RD} 2>&1 | tail -3
python3 $T/qemu-sweet.py $O/payload.bin $O/out --secs $SECS --kernel $U 2>&1 | tail -15
ls $O/out

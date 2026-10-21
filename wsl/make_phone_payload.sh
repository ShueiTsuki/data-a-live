#!/bin/bash
# make_phone_payload.sh RUNDIR [extra cmdline]: phone ADT + payload for shim/loader -> shim/loader/out/payload.bin
# ADTFLAGS=--qemu-gic NAME=outq: QEMU-GIC variant for tools/qemu_loader.py into shim/loader/outq/
set -e
R=$1; EXTRA=${2:-}; N=${NAME:-out}
P=/mnt/d/DeepSeekHarness/claude/ios_pandora; T=/opt/work/tools; mkdir -p $T
for f in adt.py mkadt.py mkpayload.py; do tr -d '\r' < $P/ref/ios-sweet/tools/$f > $T/$f; done
tr -d '\r' < $P/tools/mkadt_pandora.py > $T/mkadt_pandora.py
O=$R/phone-$N; rm -rf $O; mkdir -p $O
python3 $T/mkadt_pandora.py $O/phone.adt ${ADTFLAGS:-} 2>&1 | tail -2
python3 $T/mkpayload.py $R/xnu-kernel/kernel.development.pandora $O/phone.adt $O/payload.bin \
  --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0 $EXTRA" \
  --ramdisk /opt/work/init/sweet-init 2>&1 | tail -4
mkdir -p $P/shim/loader/$N
cp $O/payload.bin $P/shim/loader/$N/payload.bin
sha256sum $R/xnu-kernel/kernel.development.pandora $O/payload.bin

#!/bin/bash
# make_phone_payload.sh RUNDIR [extra cmdline]: phone ADT + payload for shim/loader -> shim/loader/out/payload.bin
# ADTFLAGS=--qemu-gic NAME=outq: QEMU-GIC variant for tools/qemu_loader.py into shim/loader/outq/
# CPUS=N: cores to start (default 1); DEBUG=0x8: panics restart (0xc waits, DB_NMI)
# KERNEL=FILE: this kernel instead of RUNDIR's (e.g. tools/mkprelink.py output with original kexts)
# RD=ufsroot0: root on UFS (with ADTFLAGS --root-extents for a pinned userdata file), no RAM
#   disk; default RD=md0 with the sweet-init RAM disk
# TRUSTCACHE="FILE..." : IPSW trust caches (first = static) into the payload and the ADT
#   (tools/addtrustcache.py), e.g. /opt/work/ipsw/Firmware/087-86698-044.dmg.trustcache
set -e
R=$1; EXTRA=${2:-}; N=${NAME:-out}; CPUS=${CPUS:-1}; RD=${RD:-md0}; DEBUG=${DEBUG:-0xc}
RAMDISK=(); [ "$RD" = md0 ] && RAMDISK=(--ramdisk /opt/work/init/sweet-init)
P=/mnt/d/DeepSeekHarness/claude/ios_pandora; T=/opt/work/tools; mkdir -p $T
for f in adt.py mkadt.py mkpayload.py; do tr -d '\r' < $P/ref/ios-sweet/tools/$f > $T/$f; done
tr -d '\r' < $P/tools/mkadt_pandora.py > $T/mkadt_pandora.py
tr -d '\r' < $P/tools/addtrustcache.py > $T/addtrustcache.py
# Kernel VA that maps to phys_base: 32 MB below the lowest segment instead of the mach header,
# so original kexts that tools/mkprelink.py placed below our kernel are copied too (same value
# as before for a kernel without them).
sed -i "s/virt_base = text\['vmaddr'\] & ~(0x2000000 - 1)/virt_base = min(s['vmaddr'] for s in segs) \& ~(0x2000000 - 1)/" $T/mkpayload.py
grep -q "virt_base = min(s\['vmaddr'\] for s in segs) & ~(0x2000000 - 1)" $T/mkpayload.py
K=${KERNEL:-$R/xnu-kernel/kernel.development.pandora}
O=$R/phone-$N; rm -rf $O; mkdir -p $O
python3 $T/mkadt_pandora.py $O/phone.adt ${ADTFLAGS:-} 2>&1 | tail -2
python3 $T/mkpayload.py $K $O/phone.adt $O/payload.bin \
  --cmdline "debug=$DEBUG serial=3 cpus=$CPUS keepsyms=1 -enable_kprintf_spam -noprogress rd=$RD $EXTRA" \
  "${RAMDISK[@]}" 2>&1 | tail -4
if [ -n "${TRUSTCACHE:-}" ]; then
	python3 $T/addtrustcache.py $O/payload.bin $O/payload-tc.bin $TRUSTCACHE
	mv $O/payload-tc.bin $O/payload.bin
fi
mkdir -p $P/shim/loader/$N
cp $O/payload.bin $P/shim/loader/$N/payload.bin
sha256sum $K $O/payload.bin

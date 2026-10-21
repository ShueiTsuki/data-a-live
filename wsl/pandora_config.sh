#!/bin/bash
# Make patch 0102: PANDORA machine config = FUXI code paths + 4 KiB pages.
set -euo pipefail
cd /opt/work/tree
git checkout -q -- . && git clean -qfd
sed -i 's/^SUPPORTED_ARM64_MACHINE_CONFIGS = \(.*\) FUXI$/SUPPORTED_ARM64_MACHINE_CONFIGS = \1 FUXI PANDORA/' makedefs/MakeInc.def
sed -i '/^MACHINE_FLAGS_ARM64_FUXI = /a \
# Xiaomi 17 Pro (pandora, SM8850): Fuxi providers with 4 KiB pages (no 16K granule on Oryon).\
MACHINE_FLAGS_ARM64_PANDORA = -DARM64_BOARD_CONFIG_SWEET -DFUXI=1 -DPANDORA=1 -march=armv8.2-a+crypto' makedefs/MakeInc.def
sed -i 's/\$(filter SWEET FUXI,\$(CURRENT_MACHINE_CONFIG))/$(filter SWEET FUXI PANDORA,$(CURRENT_MACHINE_CONFIG))/' makedefs/MakeInc.def libkern/conf/Makefile.template bsd/conf/Makefile.template
sed 's/^# fuxi (Xiaomi 13, SM8550), single-CPU RAM bring-up:/# pandora (Xiaomi 17 Pro, SM8850), 4 KiB pages, otherwise as fuxi:/' config/MASTER.arm64.fuxi > config/MASTER.arm64.pandora
python3 - <<'PY'
p='pexpert/pexpert/arm64/SWEET.h'
t=open(p).read()
old='#define __ARM_16K_PG__            1\n'
assert t.count(old)==1
t=t.replace(old,'#if defined(PANDORA)\n/* SM8850 (Oryon) has no 16K translation granule: 4 KiB pages like BCM2837. */\n#define __ARM64_PMAP_SUBPAGE_L1__ 1\n#else\n#define __ARM_16K_PG__            1\n#endif\n')
open(p,'w').write(t)
PY
git diff --stat
git diff > /opt/work/0102-pandora-machine-config-4k.patch
grep -c '' /opt/work/0102-pandora-machine-config-4k.patch

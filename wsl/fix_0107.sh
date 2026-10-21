#!/bin/bash
# Patch 0107: PANDORA uses the iOS address-space layout (no ARM_LARGE_MEMORY) while built through the macOS flow.
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p='makedefs/MakeInc.def'
t=open(p).read()
old='''# Use ARM_LARGE_MEMORY config for all MacOSX targets.
ifneq ($(filter $(PLATFORM),MacOSX),)
LARGE_MEMORY_DEFINE=-DARM_LARGE_MEMORY=1
ARM64_PLKSEG_ADDR  =0xfffffe0004004000
ARM64_LINK_ADDR    =0xfffffe0007004000
endif
'''
assert t.count(old)==1
new='''# Use ARM_LARGE_MEMORY config for all MacOSX targets.
# PANDORA (4 KiB pages): the boot page tables cover only the top 256 GiB of kernel
# VA (T1SZ_BOOT 26), which excludes the large-memory link address. Keep the iOS
# layout there, as the 4K BCM2837/UniXNU kernels do.
ifneq ($(filter $(PLATFORM),MacOSX),)
ifeq ($(filter PANDORA,$(CURRENT_MACHINE_CONFIG)),)
LARGE_MEMORY_DEFINE=-DARM_LARGE_MEMORY=1
ARM64_PLKSEG_ADDR  =0xfffffe0004004000
ARM64_LINK_ADDR    =0xfffffe0007004000
endif
endif
'''
t=t.replace(old,new); open(p,'w').write(t)
p='osfmk/mach/arm/vm_param.h'
t=open(p).read()
old='''#if defined(XNU_PLATFORM_MacOSX) || defined(XNU_PLATFORM_DriverKit)
#define MACH_VM_MAX_ADDRESS_RAW 0x00007FFFFE000000ULL
#else'''
assert t.count(old)==1
new='''#if (defined(XNU_PLATFORM_MacOSX) || defined(XNU_PLATFORM_DriverKit)) && !defined(PANDORA)
#define MACH_VM_MAX_ADDRESS_RAW 0x00007FFFFE000000ULL
#else
/* iOS layout; also PANDORA, whose iOS userland runs on a 4 KiB macOS-flow kernel. */'''
t=t.replace(old,new); open(p,'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0107-pandora-ios-address-layout.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0107
grep -n 'CURRENT_MACHINE_CONFIG' makedefs/MakeInc.def | head -5

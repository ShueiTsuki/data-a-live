#!/bin/bash
# Patch 0106: build fixes for the 4 KiB PANDORA kernel (found by CI run 39663063820).
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p='osfmk/arm/pmap/pmap.c'
t=open(p).read()
old='''#elif (ARM_PGSHIFT == 12)
/**
 * Ensure that 64-bit devices using 4K pages can nest the commpage completely
 * above the maximum userspace VA.
 */
static_assert((_COMM_PAGE64_BASE_ADDRESS & ~ARM_TT_L1_OFFMASK) >= MACH_VM_MAX_ADDRESS);
'''
assert t.count(old)==1
new='''#elif (ARM_PGSHIFT == 12)
#if defined(XNU_PLATFORM_MacOSX)
/**
 * PANDORA: a 4K kernel built with the macOS platform layout (MACH_VM_MAX_ADDRESS
 * 0x7FFFFE000000), a combination Apple never shipped. As on 16K macOS, the
 * commpage then lies inside the userspace VA range and is protected by the
 * "commpage nesting" reserved region (vm_reserved_regions[], 1 GiB at
 * _COMM_PAGE64_NESTING_START). With 4K pages the nested table is one L1 entry,
 * so the reservation must cover exactly that L1 block.
 */
static_assert(((_COMM_PAGE64_NESTING_START & ARM_TT_L1_OFFMASK) == 0) &&
    (_COMM_PAGE64_NESTING_SIZE == ARM_TT_L1_SIZE) &&
    ((_COMM_PAGE64_BASE_ADDRESS & ~ARM_TT_L1_OFFMASK) == _COMM_PAGE64_NESTING_START));
#else
/**
 * Ensure that 64-bit devices using 4K pages can nest the commpage completely
 * above the maximum userspace VA.
 */
static_assert((_COMM_PAGE64_BASE_ADDRESS & ~ARM_TT_L1_OFFMASK) >= MACH_VM_MAX_ADDRESS);
#endif
'''
t=t.replace(old,new); open(p,'w').write(t)
p='bsd/dev/fuxi_adreno.c'
t=open(p).read()
old='static ppnum_t fg_ppns[FG_SOURCE_BYTES/PAGE_SIZE+1];'
assert t.count(old)==1
t=t.replace(old,'/* PAGE_SIZE is not a constant on 4K kernels; size for the smallest page. */\nstatic ppnum_t fg_ppns[FG_SOURCE_BYTES/PAGE_MIN_SIZE+1];')
open(p,'w').write(t)
PY
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0106-pandora-4k-build-fixes.patch
git diff --stat
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0106

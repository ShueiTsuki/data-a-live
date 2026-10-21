#!/bin/bash
cd /opt/work/tree
grep -n -E 'ARM_LARGE_MEMORY|VM_KERNEL_LINK_ADDRESS' makedefs/MakeInc.def | head
echo ===
grep -n -B2 -A12 'if __ARM_KERNEL_PROTECT__\|define T1SZ_BOOT\|define T0SZ_BOOT' osfmk/arm64/proc_reg.h | grep -E 'define T[01]SZ|ARM_LARGE_MEMORY|ARM_PGSHIFT|__ARM_16K_PG__|#if|#else|#elif|#endif' | head -40

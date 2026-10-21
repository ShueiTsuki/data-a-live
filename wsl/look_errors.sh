#!/bin/bash
cd /opt/work/tree
sed -n 10990,11006p osfmk/arm/pmap/pmap.c
echo ====
sed -n 218,228p bsd/dev/fuxi_adreno.c
echo ====
sed -n 40,80p osfmk/mach/arm/vm_param.h
echo ====
grep -n -E 'define (MACH_VM_MAX_ADDRESS|VM_MAX_ADDRESS|VM_MAX_PAGE_ADDRESS|ARM64_MAX_OFFSET|MACH_VM_MAX_ADDRESS_RAW)' osfmk/mach/arm/vm_param.h osfmk/mach/arm/*.h osfmk/arm64/*.h 2>/dev/null | head -20

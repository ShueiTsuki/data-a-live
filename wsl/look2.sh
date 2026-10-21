#!/bin/bash
cd /opt/work/tree
sed -n 128,145p osfmk/mach/arm/vm_param.h
echo ===
sed -n 10975,10992p osfmk/arm/pmap/pmap.c
echo ===
grep -rn -E '_COMM_PAGE64_BASE_ADDRESS\s' osfmk/arm/cpu_capabilities.h osfmk/arm64/*.h 2>/dev/null | head
grep -n 'XNU_TARGET_OS_OSX\|TARGET_OS_OSX' /opt/work/run2/run2.full.log | head -2

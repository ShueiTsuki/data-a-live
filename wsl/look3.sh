#!/bin/bash
cd /opt/work/tree
sed -n 150,215p osfmk/arm/cpu_capabilities.h
echo ===
grep -n -E 'define PAGE_(MIN|MAX)_(SIZE|SHIFT)' osfmk/mach/arm/vm_param.h
grep -rn -E 'COMM_PAGE64_NESTING|commpage' osfmk/vm/vm_map.c | head -8

#!/bin/bash
cd /opt/work/tree
sed -n 318,350p makedefs/MakeInc.def
grep -rn 'ARM_LARGE_MEMORY' osfmk/mach/arm/vm_param.h osfmk/arm64/proc_reg.h | head -12

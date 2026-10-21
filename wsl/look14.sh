#!/bin/bash
cd /opt/work/tree
sed -n 213,240p osfmk/mach/arm/vm_param.h
grep -rn -E 'define ZONE_MAP_VA_SIZE|define ZONE_MAP_MAX|ZONE_MAP_VIRTUAL_SIZE|zone_map_size =|zone_get_map_size|ZSECURITY_CONFIG_KERNEL_PTR_SPLIT|define VM_MAP_KERNEL_.*SIZE|KERNEL_PMAP_HEAP_RANGE' osfmk/kern/zalloc.c osfmk/kern/zalloc_internal.h osfmk/mach/vm_param.h osfmk/vm/vm_kern.c osfmk/kern/zalloc.h 2>/dev/null | head -20

#!/bin/bash
cd /opt/work/tree
n=$(grep -n '^kmem_add_extra_claims' osfmk/vm/vm_kern.c | cut -d: -f1)
sed -n "$((n-5)),$((n+45))p" osfmk/vm/vm_kern.c
grep -n -E 'kmem_ptr_ranges|KMEM_PTR_RANGE|kmem_range_get_ptr|ZONE_MAP_|largest_free_size =' osfmk/vm/vm_kern.c | head -20
grep -n -B3 -A25 'if defined(ARM_LARGE_MEMORY)' osfmk/mach/arm/vm_param.h | sed -n 1,60p

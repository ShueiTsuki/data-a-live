#!/bin/bash
cd /opt/work/tree
grep -rn '_COMM_PAGE64_NESTING_START' --include=*.c --include=*.h osfmk bsd | head
n=$(grep -n 'vm_reserved_regions\[\]' osfmk/vm/vm_map.c | head -1 | cut -d: -f1); sed -n "${n},$((n+30))p" osfmk/vm/vm_map.c

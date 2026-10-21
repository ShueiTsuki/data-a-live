#!/bin/bash
cd /opt/work/tree
sed -n 155,175p osfmk/kern/zalloc.c
grep -rn -E 'KMEM_RANGE_ID|kmem_meta_size|kc_size *=' osfmk/vm/vm_kern.c | sed -n 1,10p
grep -rn -E 'STARTUP_ARG|KMEM_RANGE_REGISTER_STATIC|KMEM_RANGE_REGISTER_DYNAMIC' --include=*.c osfmk bsd iokit | grep -v '^osfmk/vm/vm_kern.c:.*define' | grep -c KMEM_RANGE_REGISTER
grep -rn -h 'KMEM_RANGE_REGISTER_\(STATIC\|DYNAMIC\)(' --include=*.c osfmk bsd iokit san security | head -20

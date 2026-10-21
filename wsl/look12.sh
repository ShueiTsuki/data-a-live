#!/bin/bash
cd /opt/work/tree
n=$(grep -n 'failing for claim' osfmk/vm/vm_kern.c | head -1 | cut -d: -f1)
sed -n "$((n-80)),$((n+5))p" osfmk/vm/vm_kern.c

#!/bin/bash
cd /opt/work/tree
sed -n 688,712p osfmk/vm/vm_compressor.c
grep -n 'VM_COMPRESSOR_MAX_POOL_SIZE' osfmk/vm/vm_compressor.c | head

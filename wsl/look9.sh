#!/bin/bash
cd /opt/work/tree
sed -n 1820,1880p osfmk/arm64/arm_vm_init.c
echo ====
grep -n -E 'fbcon|map|PE_state.video|v_baseAddr|ml_io_map|io_map' pexpert/arm/pe_serial.c | grep -i -E 'sweet|fbcon|fb' | head -20

#!/bin/bash
cd /opt/work/tree
grep -rn -E 'ARM_TT_L2|TWIG|sweet_early_fb|SWEET_EARLY_VA|fb_va|FB_VA' osfmk/arm64/sweet_early.c pexpert/arm/pe_serial.c osfmk/arm64/arm_vm_init.c 2>/dev/null | grep -i -E 'sweet|fb|early' | head -30

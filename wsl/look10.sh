#!/bin/bash
cd /opt/work/tree
sed -n 700,745p pexpert/arm/pe_serial.c
grep -n -A25 'sweet-fbcon' pexpert/arm/pe_serial.c | sed -n 30,90p | grep -n -E 'ml_io_map|io_map|fbcon_base|reg|size|width|stride' | head -20

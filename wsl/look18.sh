#!/bin/bash
cd /opt/work/tree
grep -n -B2 -A12 'SweetPlatformExpert' iokit/KernelConfigTables.cpp | head -50
grep -n -E 'compatible|xiaomi' /opt/work/tools/mkadt.py | head -10

#!/bin/bash
# Copy this folder's helper scripts (and tools/qemu_loader.py) to /opt/work/sh without CRs.
set -e
P=/mnt/d/DeepSeekHarness/claude/ios_pandora
mkdir -p /opt/work/sh
for f in $P/wsl/*.sh; do tr -d '\r' < "$f" > /opt/work/sh/$(basename "$f"); done
tr -d '\r' < $P/tools/qemu_loader.py > /opt/work/sh/qemu_loader.py
ls /opt/work/sh | wc -l

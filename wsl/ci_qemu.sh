#!/bin/bash
# ci_qemu.sh RUNID N [SECS] [extra cmdline]: wait for CI, fetch to /opt/work/runN, boot in QEMU, summarize.
set -u
ID=$1; N=$2; SECS=${3:-60}; EXTRA=${4:-}
W=/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl
bash $W/ci_fetch.sh $ID $N
[ -f /opt/work/run$N/xnu-kernel/kernel.development.pandora ] || { echo "no kernel"; exit 1; }
bash $W/qemu_run.sh /opt/work/run$N $SECS "$EXTRA" > /opt/work/run$N/qemu_run.out 2>&1
O=/opt/work/run$N/qemu/out
echo "== result: $(tr -d '\n ' < $O/result.json)"
echo "== exceptions: $(grep -c 'Taking exception' $O/qemu.log)"
grep -m3 -A4 'Taking exception' $O/qemu.log | grep -E 'exception|ESR|ELR|FAR'
echo "== screen (last 45 lines)"
grep -v '^\s*$' $O/fb.txt | tail -45

#!/bin/bash
# Have Turnip on freedreno's noop drm-shim compile every meta in DIR, one process each, for the
# GPU FD_CHIP_ID names (default: Adreno 840 as Linux reports it, 0x44050a31).
#   run_tu.sh DIR [CHIP_ID]
set -u
D=$1; CHIP=${2:-44050a31}; B=/opt/work/mesa_main/build; H=$(cd "$(dirname "$0")" && pwd)/vk_pipeline.py
source /opt/work/mesonenv/bin/activate
export VK_ICD_FILENAMES=/opt/work/m2vtest/tu_icd.json LD_PRELOAD=$B/src/freedreno/drm-shim/libfreedreno_noop_drm_shim.so \
	FD_CHIP_ID=$CHIP MESA_LOADER_DRIVER_OVERRIDE=msm H
one() {
	out=$(python3 $H $1 2>/tmp/tu_err_$$_$(basename $1); echo "rc=$?")
	rc=${out##*rc=}; line=$(echo "$out" | grep -E "^(PASS|FAIL)")
	if [ -n "$line" ]; then echo "$line"; else echo "CRASH $(basename $1) rc=$rc: $(grep -m1 -i -E "error|assert|abort" /tmp/tu_err_$$_$(basename $1) | cut -c1-160)"; fi
	rm -f /tmp/tu_err_$$_$(basename $1)
}
export -f one
ls $D/*.json | xargs -P 8 -I{} bash -c 'one {}' > $D/tu_$CHIP.txt
echo "chip $CHIP: PASS $(grep -c ^PASS $D/tu_$CHIP.txt) FAIL $(grep -c ^FAIL $D/tu_$CHIP.txt) CRASH $(grep -c ^CRASH $D/tu_$CHIP.txt) of $(ls $D/*.json | wc -l)"

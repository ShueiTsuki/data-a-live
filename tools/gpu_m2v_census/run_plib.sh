#!/bin/bash
# Translate every pipeline-library record (fragments paired with the specialized fixed vertex record).
T=/opt/work/m2v/target/release/metal2vulkan; D=/opt/work/m2vtest/plib; MAIN=/opt/work/m2vtest/qc_default.metallib
P=/opt/work/m2v/crates/metal2vulkan-validation/profiles/$1.json; O=/opt/work/m2vtest/plib_out_$1; rm -rf $O; mkdir -p $O
L=/mnt/c/Users/MSI-PC/AppData/Local/Temp/claude/D--DeepSeekHarness-claude/0c6fe11f-a60e-42c6-9d52-feb15149f6f0/scratchpad/m2vtest/mtlb_list.py
VREC=$(for f in $D/*.metallib; do python3 $L $f | grep -q "vertex fixed_vert_lph_gen" && echo $f; done | head -1)
export METAL2VULKAN_REPRO_DIR=$O/repro T D MAIN P O L VREC
one() {
	f=$1; read kind name < <(python3 $L $f)
	VD="--vertex-descriptor /opt/work/m2vtest/vd8.json"
	if [ "$kind" = fragment ]; then
		r=$($T $VREC $O/x.spv --profile $P --function fixed_vert_lph_gen --fragment $name --fragment-library $f --color-format 0:BGRA8Unorm $VD --no-spirv-val 2>&1 | tail -1)
	else
		r=$($T $f $O/x.spv --profile $P --function $name --fragment path_clear_frag_lpf --fragment-library $MAIN --color-format 0:BGRA8Unorm $VD --no-spirv-val 2>&1 | tail -1)
	fi
	echo "$(basename $f) $kind $name | $r"
}
export -f one
ls $D/*.metallib | xargs -P 8 -I{} bash -c 'one {}' > $O/results.txt
echo "== $1: $(grep -c PASS $O/results.txt) PASS of $(wc -l < $O/results.txt) (vertex record: $(basename $VREC))"
grep -v PASS $O/results.txt | sed -E 's/^[^|]*\| //; s/[0-9]+/N/g' | cut -c1-170 | sort | uniq -c | sort -rn | head -6

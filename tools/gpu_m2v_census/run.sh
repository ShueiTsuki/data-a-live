#!/bin/bash
# Translate every QuartzCore entry point with one device profile; one result line per function.
M=/opt/work/m2vtest/qc_default.metallib; T=/opt/work/m2v/target/release/metal2vulkan
P=/opt/work/m2v/crates/metal2vulkan-validation/profiles/$1.json; O=/opt/work/m2vtest/out_$1; rm -rf $O; mkdir -p $O
export METAL2VULKAN_REPRO_DIR=$O/repro
one() {
	kind=$1 name=$2
	case $kind in
	fragment) args="--function std_vert0_lpf --fragment $name --color-format 0:BGRA8Unorm --vertex-descriptor /opt/work/m2vtest/vd8.json" ;;
	vertex) args="--function $name --fragment path_clear_frag_lpf --color-format 0:BGRA8Unorm --vertex-descriptor /opt/work/m2vtest/vd8.json" ;;
	kernel) args="--function $name" ;;
	esac
	r=$($T $M $O/$name.spv --profile $P $args --no-spirv-val 2>&1 | tail -1)
	echo "$kind $name | $r"
}
export -f one; export M T P O
cat /opt/work/m2vtest/funcs.txt | xargs -P 8 -L 1 bash -c 'one $0 $1' > $O/results.txt
echo "== $1: $(grep -c "PASS" $O/results.txt) PASS of $(wc -l < $O/results.txt)"
sed -E 's/^[a-z]+ [^ ]+ \| //; s/[0-9]+/N/g' $O/results.txt | cut -c1-140 | sort | uniq -c | sort -rn | head -12

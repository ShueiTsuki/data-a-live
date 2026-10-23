#!/bin/bash
# Second pass over lavapipe failures: find a matching vertex for each fragment, give extra
# colour attachments, and compile tile kernels as tile pipelines.
M=/opt/work/m2vtest/qc_default.metallib; T=/opt/work/m2v/target/release/metal2vulkan
P=/opt/work/m2v/crates/metal2vulkan-validation/profiles/lavapipe.json; O=/opt/work/m2vtest/out_retry; rm -rf $O; mkdir -p $O
export METAL2VULKAN_REPRO_DIR=/proc/no-repro M T P O
VD="--vertex-descriptor /opt/work/m2vtest/vd8.json"
CF="--color-format 0:BGRA8Unorm --color-format 1:BGRA8Unorm --color-format 2:BGRA8Unorm --color-format 3:BGRA8Unorm"
export VD CF
try_frag() {
	f=$1; last=""
	for v in $(grep "^vertex " /opt/work/m2vtest/funcs.txt | cut -d" " -f2); do
		for cf in "--color-format 0:BGRA8Unorm" "$CF"; do
			r=$($T $M $O/$f.spv --profile $P --function $v --fragment $f $cf $VD --no-spirv-val 2>&1 | tail -1)
			case $r in *PASS*) echo "fragment $f | PASS with $v"; return ;; esac
			last=$r
		done
	done
	echo "fragment $f | $last"
}
try_vert() {
	v=$1; last=""
	for f in $(grep "^fragment " /opt/work/m2vtest/funcs.txt | cut -d" " -f2); do
		r=$($T $M $O/$v.spv --profile $P --function $v --fragment $f --color-format 0:BGRA8Unorm $VD --no-spirv-val 2>&1 | tail -1)
		case $r in *PASS*) echo "vertex $v | PASS with $f"; return ;; esac
		last=$r
	done
	echo "vertex $v | $last"
}
try_kernel() {
	k=$1
	r=$($T $M $O/$k.spv --profile $P --function $k --color-format 0:BGRA8Unorm --no-spirv-val 2>&1 | tail -1)
	echo "kernel $k | $r"
}
export -f try_frag try_vert try_kernel
grep -v PASS /opt/work/m2vtest/out_lavapipe/results.txt | awk '{print $1, $2}' > $O/todo.txt
grep "^fragment" $O/todo.txt | cut -d" " -f2 | xargs -P 8 -I{} bash -c 'try_frag {}' > $O/frag.txt
grep "^vertex" $O/todo.txt | cut -d" " -f2 | xargs -P 8 -I{} bash -c 'try_vert {}' > $O/vert.txt
grep "^kernel" $O/todo.txt | cut -d" " -f2 | xargs -P 8 -I{} bash -c 'try_kernel {}' > $O/kern.txt
cat $O/frag.txt $O/vert.txt $O/kern.txt > $O/results.txt
echo "retried $(wc -l < $O/todo.txt): now PASS $(grep -c PASS $O/results.txt)"
grep -v PASS $O/results.txt | sed -E 's/[0-9]+/N/g' | cut -c1-200

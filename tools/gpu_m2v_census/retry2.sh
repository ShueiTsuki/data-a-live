#!/bin/bash
# Third pass: path_blit with three attachments; fixed_* uber shaders with zero constants.
M=/opt/work/m2vtest/qc_default.metallib; T=/opt/work/m2v/target/release/metal2vulkan
P=/opt/work/m2v/crates/metal2vulkan-validation/profiles/lavapipe.json; O=/opt/work/m2vtest/out_retry2; rm -rf $O; mkdir -p $O
export METAL2VULKAN_REPRO_DIR=$O/repro
VD="--vertex-descriptor /opt/work/m2vtest/vd8.json"
BLIT="--color-format 0:BGRA8Unorm --color-format 1:BGRA8Unorm --color-format 2:R32Float"
for f in path_blit_frag_lpf path_blit_frag_eo_lpf path_blit_frag_lph path_blit_frag_eo_lph; do
	res="none"
	for v in $(grep "^vertex " /opt/work/m2vtest/funcs.txt | cut -d" " -f2); do
		r=$($T $M $O/$f.spv --profile $P --function $v --fragment $f $BLIT $VD --no-spirv-val 2>&1 | tail -1)
		case $r in *PASS*) res="PASS with $v"; break ;; esac; res=$r
	done
	echo "fragment $f | $res"
done
# Fill function constants one by one (bool false, then uchar 0, then uchar3 0,0,0).
solve() {
	local stage=$1 fn=$2 pair=$3 consts="" tries=0 r
	while [ $tries -lt 200 ]; do
		tries=$((tries+1))
		if [ $stage = vertex ]; then
			r=$($T $M $O/$fn.spv --profile $P --function $fn --fragment $pair --color-format 0:BGRA8Unorm $VD $consts --no-spirv-val 2>&1 | tail -1)
		else
			r=$($T $M $O/$fn.spv --profile $P --function $pair --fragment $fn --color-format 0:BGRA8Unorm $VD $consts --no-spirv-val 2>&1 | tail -1)
		fi
		case $r in
		*PASS*) echo "$stage $fn | PASS ($(echo $consts | grep -o -- --constant | wc -l) constants, pair $pair)"; return ;;
		*"FunctionConstant { index: "*)
			i=$(echo "$r" | sed -E 's/.*index: ([0-9]+).*/\1/')
			if echo "$consts" | grep -q -- "--constant $i:uchar:0 "; then consts=$(echo "$consts" | sed "s/--constant $i:uchar:0 /--constant $i:uchar3:0,0,0 /")
			elif echo "$consts" | grep -q -- "--constant $i:bool:false "; then consts=$(echo "$consts" | sed "s/--constant $i:bool:false /--constant $i:uchar:0 /")
			elif echo "$consts" | grep -q -- "--constant $i:uchar3"; then echo "$stage $fn | stuck at constant $i: $r"; return
			else consts="$consts --constant $i:bool:false "; fi ;;
		*type*|*Type*)
			i=$(echo "$r" | grep -o -E "index:? ?[0-9]+|constant [0-9]+" | grep -o -E "[0-9]+" | head -1)
			if [ -n "$i" ] && echo "$consts" | grep -q -- "--constant $i:bool:false "; then consts=$(echo "$consts" | sed "s/--constant $i:bool:false /--constant $i:uchar:0 /")
			elif [ -n "$i" ] && echo "$consts" | grep -q -- "--constant $i:uchar:0 "; then consts=$(echo "$consts" | sed "s/--constant $i:uchar:0 /--constant $i:uchar3:0,0,0 /")
			else echo "$stage $fn | type issue: $r"; return; fi ;;
		*) echo "$stage $fn | $r"; return ;;
		esac
	done
	echo "$stage $fn | gave up"
}
for v in fixed_vert_lph_spc fixed_vert_lph_gen fixed_vert_lpf_gen fixed_vert_lpf_spc; do solve vertex $v path_clear_frag_lpf; done
for f in fixed_frag_lph_cph fixed_frag_lph_cpf; do solve fragment $f fixed_vert_lph_gen; done

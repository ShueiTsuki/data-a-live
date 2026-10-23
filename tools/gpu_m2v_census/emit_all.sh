#!/bin/bash
# Translate every QuartzCore entry point and pipeline-library record once more, keeping the SPIR-V
# and a --emit-meta file per pipeline, using the first probe configuration that passes (the same
# pairings, attachments and zero constants as run.sh/retry*.sh/run_plib.sh). The metas feed
# vk_pipeline.py, which has Turnip compile them for a real GPU model.
#
#   emit_all.sh OUTDIR
set -u
W=/opt/work/m2vtest; T=/opt/work/m2v/target/release/metal2vulkan; MAIN=$W/qc_default.metallib
P=${M2V_PROFILE:-/opt/work/m2v/crates/metal2vulkan-validation/profiles/lavapipe.json}
L=$(cd "$(dirname "$0")" && pwd)/mtlb_list.py
O=$1; rm -rf "$O"; mkdir -p "$O"
export METAL2VULKAN_REPRO_DIR=/proc/no-repro T MAIN P O L W
VD="--vertex-descriptor $W/vd8.json"
CF1="--color-format 0:BGRA8Unorm"
CF2="--color-format 0:BGRA8Unorm --color-format 1:BGRA8Unorm"
CF3="--color-format 0:BGRA8Unorm --color-format 1:BGRA8Unorm --color-format 2:R32Float"
CF4="--color-format 0:BGRA8Unorm --color-format 1:BGRA8Unorm --color-format 2:BGRA8Unorm --color-format 3:BGRA8Unorm"
export VD CF1 CF2 CF3 CF4

# The constants a function needs, filled with zeros (bool false, uchar 0, uchar3 0,0,0) one by one.
consts_for() {
	local consts="" r i tries=0
	while [ $tries -lt 200 ]; do
		tries=$((tries+1))
		r=$("$@" $consts --no-spirv-val 2>&1 | tail -1)
		case $r in
		*"FunctionConstant { index: "*)
			i=$(echo "$r" | sed -E 's/.*index: ([0-9]+).*/\1/')
			if echo "$consts" | grep -q -- "--constant $i:uchar:0 "; then consts=$(echo "$consts" | sed "s/--constant $i:uchar:0 /--constant $i:uchar3:0,0,0 /")
			elif echo "$consts" | grep -q -- "--constant $i:bool:false "; then consts=$(echo "$consts" | sed "s/--constant $i:bool:false /--constant $i:uchar:0 /")
			else consts="$consts --constant $i:bool:false "; fi ;;
		*FunctionConstantType*)
			i=$(echo "$r" | grep -o -E "index: [0-9]+" | head -1 | grep -o -E "[0-9]+")
			if [ -n "$i" ] && echo "$consts" | grep -q -- "--constant $i:bool:false "; then consts=$(echo "$consts" | sed "s/--constant $i:bool:false /--constant $i:uchar:0 /")
			elif [ -n "$i" ] && echo "$consts" | grep -q -- "--constant $i:uchar:0 "; then consts=$(echo "$consts" | sed "s/--constant $i:uchar:0 /--constant $i:uchar3:0,0,0 /")
			else break; fi ;;
		*) break ;;
		esac
	done
	echo "$consts"
}
export -f consts_for

# One main-library entry point: try pairings and attachment sets until one passes.
main_one() {
	kind=$1 name=$2; out=$O/main_$name
	try() { r=$("$@" --emit-meta $out.json 2>&1 | tr '
' ' '); case $r in *PASS*) echo "main $kind $name | PASS"; return 0 ;; esac; return 1; }
	case $kind in
	kernel)
		try $T $MAIN $out.spv --profile $P --function $name && return
		try $T $MAIN $out.spv --profile $P --function $name $CF1 && return ;;
	fragment)
		for v in std_vert0_lpf fixed_vert_lph_gen $(grep "^vertex " $W/funcs.txt | cut -d" " -f2); do
			for cf in "$CF1" "$CF2" "$CF3" "$CF4"; do
				base="$T $MAIN $out.spv --profile $P --function $v --fragment $name $cf $VD"
				c=$(consts_for $base)
				try $base $c && return
			done
		done ;;
	vertex)
		for f in path_clear_frag_lpf $(grep "^fragment " $W/funcs.txt | cut -d" " -f2); do
			base="$T $MAIN $out.spv --profile $P --function $name --fragment $f $CF1 $VD"
			c=$(consts_for $base)
			try $base $c && return
		done ;;
	esac
	echo "main $kind $name | $r"
}
export -f main_one

plib_one() {
	f=$1; read kind name < <(python3 $L $f); out=$O/plib_$(basename $f .metallib)
	try() { r=$("$@" --emit-meta $out.json 2>&1 | tr '
' ' '); case $r in *PASS*) echo "plib $kind $(basename $f) $name | PASS"; return 0 ;; esac; return 1; }
	if [ "$kind" = fragment ]; then
		VREC=$(cat $O/vrec)
		for cf in "$CF1" "$CF2" "$CF3" "$CF4"; do
			try $T $VREC $out.spv --profile $P --function fixed_vert_lph_gen --fragment $name --fragment-library $f $cf $VD && return
		done
	else
		try $T $f $out.spv --profile $P --function $name --fragment path_clear_frag_lpf --fragment-library $MAIN $CF1 $VD && return
	fi
	echo "plib $kind $(basename $f) $name | $r"
}
export -f plib_one

for f in $W/plib/*.metallib; do python3 $L $f | grep -q "vertex fixed_vert_lph_gen" && { echo $f > $O/vrec; break; }; done
xargs -P 8 -L 1 bash -c 'main_one $0 $1' < $W/funcs.txt > $O/main.txt
ls $W/plib/*.metallib | xargs -P 8 -I{} bash -c 'plib_one {}' > $O/plib.txt
echo "main: $(grep -c PASS $O/main.txt) of $(wc -l < $O/main.txt); plib: $(grep -c PASS $O/plib.txt) of $(wc -l < $O/plib.txt); metas: $(ls $O/*.json | wc -l)"
grep -v PASS $O/main.txt $O/plib.txt | head

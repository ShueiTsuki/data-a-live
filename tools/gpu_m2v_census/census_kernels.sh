#!/bin/bash
# Census: translate every compute kernel of every metallib listed in LIST (one path per line) for
# PROFILE, in parallel per library, keeping SPIR-V + meta for run_tu.sh. Kernels needing function
# constants get them filled with zeros (bool false, then uchar 0, uchar3 0,0,0) as in emit_all.sh.
#   census_kernels.sh PROFILE OUTDIR LIST
set -u
P=$1; O=$2; LIST=$3
H=$(cd "$(dirname "$0")" && pwd)
rm -rf "$O"; mkdir -p "$O"
# Candidate constant types, tried in this order (TYPE=zero value).
TYPES="bool=false uchar=0 uchar2=0,0 uchar3=0,0,0 uchar4=0,0,0,0 ushort=0 ushort2=0,0 ushort3=0,0,0 ushort4=0,0,0,0 uint=0 uint2=0,0 uint3=0,0,0 uint4=0,0,0,0 int=0 int2=0,0 int3=0,0,0 int4=0,0,0,0 short=0 char=0 float=0 float2=0,0 float3=0,0,0 float4=0,0,0,0 half=0 half2=0,0 half3=0,0,0 half4=0,0,0,0"
export TYPES P O H T=/opt/work/m2v/target/release/metal2vulkan METAL2VULKAN_REPRO_DIR=/proc/no-repro  # no repro bundles: a census writes thousands (45 GB once)
one_lib() {
	lib=$1; tag=$(echo "$lib" | sed -E 's|^/mnt/iosgpu/||; s|[^A-Za-z0-9]+|_|g')
	python3 $H/mtlb_list.py "$lib" 2>/dev/null | awk '$1=="kernel"{print $2}' | while read k; do
		consts=""
		for try in $(seq 1 400); do
			r=$($T "$lib" $O/${tag}__$k.spv --profile $P --function $k $consts --emit-meta $O/${tag}__$k.json 2>&1 | tr '\n' ' ')
			case $r in
			*PASS*) echo "$tag $k | PASS"; continue 2 ;;
			*"FunctionConstant { index: "*)
				i=$(echo "$r" | sed -E 's/.*FunctionConstant \{ index: ([0-9]+).*/\1/'); consts="$consts --constant $i:bool:false " ;;
			*FunctionConstantType*)
				# Wrong type guessed for this constant: move to the next candidate type.
				i=$(echo "$r" | grep -o -E "index: [0-9]+" | head -1 | grep -o -E "[0-9]+")
				cur=$(echo "$consts" | grep -o -E -- "--constant $i:[a-z0-9]+:[^ ]+ " | head -1)
				curty=$(echo "$cur" | cut -d: -f2)
				next=""; found=0
				for ty in $TYPES; do
					[ $found = 1 ] && { next=$ty; break; }
					[ "${ty%%=*}" = "$curty" ] && found=1
				done
				[ -z "$next" ] && break
				consts=$(echo "$consts" | sed "s/$cur/--constant $i:${next%%=*}:${next#*=} /") ;;
			*) break ;;
			esac
		done
		rm -f $O/${tag}__$k.json
		echo "$tag $k | $(echo "$r" | grep -o 'FALLBACK.*' | cut -c1-160)"
	done
}
export -f one_lib
xargs -P 8 -I{} bash -c 'one_lib "{}"' < "$LIST" > $O/translate.txt
echo "kernels translated: $(grep -c '| PASS' $O/translate.txt) PASS of $(wc -l < $O/translate.txt)"
grep -v '| PASS' $O/translate.txt | sed -E 's/^[^|]*\| //; s/[0-9]+/N/g' | sort | uniq -c | sort -rn | head -12

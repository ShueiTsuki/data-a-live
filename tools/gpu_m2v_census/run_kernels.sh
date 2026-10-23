#!/bin/bash
# Translate every kernel (compute) function of the given metallibs with metal2vulkan for a profile
# and keep SPIR-V + meta, for run_tu.sh to compile with Turnip.
#   run_kernels.sh PROFILE.json OUTDIR LIB.metallib...
set -u
T=/opt/work/m2v/target/release/metal2vulkan; P=$1; O=$2; shift 2
L=$(cd "$(dirname "$0")" && pwd)/mtlb_list.py
rm -rf "$O"; mkdir -p "$O"; export METAL2VULKAN_REPRO_DIR=/proc/no-repro
for lib in "$@"; do
	b=$(basename $lib .metallib)
	python3 $L $lib 2>/dev/null | awk '$1=="kernel"{print $2}' | while read k; do
		r=$($T $lib $O/${b}__$k.spv --profile $P --function $k --emit-meta $O/${b}__$k.json 2>&1 | tr '\n' ' ')
		case $r in *PASS*) echo "$b $k | PASS" ;; *) echo "$b $k | $(echo $r | grep -o 'FALLBACK.*' | cut -c1-150)"; rm -f $O/${b}__$k.json ;; esac
	done
done > $O/translate.txt
echo "translated: $(grep -c PASS $O/translate.txt) PASS of $(wc -l < $O/translate.txt)"
grep -v PASS $O/translate.txt | sed -E 's/^[^|]*\| //; s/[0-9]+/N/g' | sort | uniq -c | sort -rn | head -8

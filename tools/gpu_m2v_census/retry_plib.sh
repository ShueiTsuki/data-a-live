#!/bin/bash
# Retry the colour-attachment failures with several attachment layouts.
T=/opt/work/m2v/target/release/metal2vulkan; O=/opt/work/m2vtest/plib_out_lavapipe; VREC=/opt/work/m2vtest/plib/rec000.metallib
P=/opt/work/m2v/crates/metal2vulkan-validation/profiles/lavapipe.json
VD="--vertex-descriptor /opt/work/m2vtest/vd8.json"
pass=0; fail=0
while read rec kind name rest; do
	ok=""
	for cf in "--color-format 0:BGRA8Unorm --color-format 1:BGRA8Unorm" \
	          "--color-format 0:BGRA8Unorm --color-format 1:BGRA8Unorm --color-format 2:R32Float" \
	          "--color-format 0:BGRA8Unorm --color-format 1:BGRA8Unorm --color-format 2:BGRA8Unorm --color-format 3:BGRA8Unorm" \
	          "--color-format 0:BGRA8Unorm --color-format 1:R32Float" \
	          "--color-format 0:BGRA8Unorm --color-format 1:RGBA16Float"; do
		r=$($T $VREC /tmp/y.spv --profile $P --function fixed_vert_lph_gen --fragment $name --fragment-library /opt/work/m2vtest/plib/$rec $cf $VD --no-spirv-val 2>&1 | tail -1)
		case $r in *PASS*) ok="$cf"; break ;; esac
	done
	if [ -n "$ok" ]; then pass=$((pass+1)); echo "$rec PASS with $ok" >> /tmp/plib_retry.txt; else fail=$((fail+1)); echo "$rec $name | $r" >> /tmp/plib_retry.txt; fi
done < <(grep -v PASS $O/results.txt)
echo "retry: $pass PASS, $fail still failing"
sed -E 's/rec[0-9]+ //' /tmp/plib_retry.txt | sort | uniq -c | sort -rn | head -5; rm -f /tmp/plib_retry.txt

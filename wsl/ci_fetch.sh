#!/bin/bash
# Wait for a CI run, then download the kernel into /opt/work/runN. Usage: ci_fetch.sh RUNID N [all]
# Only the xnu-kernel artifact by default (the buildtree and the 30 MB log are only needed for
# local per-file checks or a failed build); "all" downloads everything as before.
set -u
ID=$1; N=$2; WHAT=${3:-kernel}; D=/opt/work/run$N
while :; do
	st=$(gh run view $ID -R ShueiTsuki/ios-pandora --json status,conclusion --jq '.status+" "+.conclusion' 2>/dev/null)
	case $st in completed*) break ;; esac
	sleep 10
done
echo "conclusion: ${st#completed }  duration_ms: $(gh api repos/ShueiTsuki/ios-pandora/actions/runs/$ID/timing --jq .run_duration_ms)"
rm -rf $D && mkdir -p $D && cd $D
if [ "$WHAT" = all ]; then
	gh run download $ID -R ShueiTsuki/ios-pandora -D . > /dev/null 2>&1
else
	gh run download $ID -R ShueiTsuki/ios-pandora -n xnu-kernel -D xnu-kernel > /dev/null 2>&1
fi
if [ "${st#completed }" != success ] || [ "$WHAT" = all ]; then
	gh run view $ID -R ShueiTsuki/ios-pandora --log > run$N.full.log 2>&1
	grep -E ' error: |Undefined symbols|ld: error' run$N.full.log | grep -v -E 'patch does not apply|already exists|Skipping patch' | sed 's/.*Z //' | head -20
fi
ls xnu-kernel 2>/dev/null; cat xnu-kernel/SHA256SUMS 2>/dev/null

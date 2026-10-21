#!/bin/bash
# Wait for a CI run, then download artifacts + log into /opt/work/runN. Usage: ci_fetch.sh RUNID N
set -u
ID=$1; N=$2; D=/opt/work/run$N
gh run watch $ID -R ShueiTsuki/ios-pandora --interval 60 > /dev/null 2>&1
echo "conclusion: $(gh run view $ID -R ShueiTsuki/ios-pandora --json conclusion --jq .conclusion)  duration_ms: $(gh api repos/ShueiTsuki/ios-pandora/actions/runs/$ID/timing --jq .run_duration_ms)"
rm -rf $D && mkdir -p $D && cd $D
gh run download $ID -R ShueiTsuki/ios-pandora -D . > /dev/null 2>&1
gh run view $ID -R ShueiTsuki/ios-pandora --log > run$N.full.log 2>&1
cp run$N.full.log run2.full.log.tmp 2>/dev/null; rm -f run2.full.log.tmp
ls xnu-kernel 2>/dev/null; cat xnu-kernel/SHA256SUMS 2>/dev/null
grep -E ' error: |Undefined symbols|ld: error' run$N.full.log | grep -v -E 'patch does not apply|already exists|Skipping patch' | sed 's/.*Z //' | head -20

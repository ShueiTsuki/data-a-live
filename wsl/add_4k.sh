#!/bin/bash
set -euo pipefail
U=/mnt/d/DeepSeekHarness/claude/unixnu_review/src/patches/xnu
P=/mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches
cp /opt/work/0102-pandora-machine-config-4k.patch $P/
n=103
for name in native-4k-bootstrap-capacity native-4k-user-abi native-4k-signature-hash-cache; do
  tr -d '\r' < $U/$name.patch > $P/0$n-unixnu-$name.patch; n=$((n+1))
done
cd /opt/work/tree
for p in $P/0102-*.patch $P/0103-*.patch $P/0104-*.patch $P/0105-*.patch; do
  git apply --check $p && git apply $p && echo "applied $(basename $p)"
done
git add -A && git -c user.name=t -c user.email=t@t commit -qm "pandora + unixnu 4k" && git log --oneline -1

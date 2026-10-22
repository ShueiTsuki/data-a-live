#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'makedefs/MakeInc.def'
t = open(p).read()
old = "MACHINE_FLAGS_ARM64_PANDORA = -DARM64_BOARD_CONFIG_SWEET -DFUXI=1 -DPANDORA=1"
assert t.count(old) == 1, 'flags'
# iOS 16 binaries and the dyld shared cache are signed with 16K code-signature pages
# ("disallowing unsupported code signature page shift: 14" on the 4K kernel): turn on
# UniXNU's verified 16K->4K hash cache (patch 0105).
t = t.replace(old, old + " -DXNU_NATIVE_CS_HASH_CACHE=1")
open(p, 'w').write(t)
PY
grep -n "MACHINE_FLAGS_ARM64_PANDORA" makedefs/MakeInc.def
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0124-pandora-native-4k-cs-hash-cache-on.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0124

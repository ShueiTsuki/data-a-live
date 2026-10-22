#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'bsd/dev/sweet_ufs.c'
t = open(p).read()
test = open('/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/pandora_ufs_exttest.c.txt').read()
anchor = """#if defined(FUXI)
void fuxi_storage_probe(void);
"""
assert t.count(anchor) == 1
t = t.replace(anchor, test + anchor)
old = """  if (probe==3) pandora_ufs_driver_test();
"""
assert t.count(old) == 1
t = t.replace(old, old + """  if (probe==4) pandora_ufs_extent_test();
""")
open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0121-pandora-ufs-extent-file-test.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0121

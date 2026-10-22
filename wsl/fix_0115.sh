#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'bsd/dev/sweet_ufs.c'
t = open(p).read()
gpt = open('/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/pandora_ufs_gpt.c.txt').read()
anchor = "/* Not ported to SM8850 yet:"
assert t.count(anchor) == 1
t = t.replace(anchor, gpt + anchor)
old = """ if (PE_parse_boot_argn("pandora_ufs_probe",&probe,sizeof(probe)) && probe==1) pandora_ufs_probe();"""
new = """ if (PE_parse_boot_argn("pandora_ufs_probe",&probe,sizeof(probe))) {
  if (probe==1) pandora_ufs_probe();
  if (probe==2) pandora_ufs_gpt();
 }"""
assert t.count(old) == 1
t = t.replace(old, new)
open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0115-pandora-readonly-ufs-gpt-read.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0115

#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'bsd/dev/sweet_ufs.c'
t = open(p).read()
probe = open('/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/pandora_ufs_probe.c.txt').read()
anchor = "static int\nsweet_ufs_init(void)\n{\n#if defined(FUXI)\n    uint32_t requested=0;"
assert t.count(anchor) == 1
t = t.replace(anchor, probe + "static int\nsweet_ufs_init(void)\n{\n#if defined(PANDORA)\n"
    "\tif (!pandora_ufs_ported) {\n\t\tprintf(\"PANDORA UFS: driver not ported to SM8850 yet" + chr(92) + "n\");\n"
    "\t\treturn ENXIO;\n\t}\n#endif\n#if defined(FUXI)\n    uint32_t requested=0;")
old = """void fuxi_storage_probe(void)
{
 uint32_t requested=0;"""
new = """void fuxi_storage_probe(void)
{
#if defined(PANDORA)
 uint32_t probe=0;
 if (PE_parse_boot_argn("pandora_ufs_probe",&probe,sizeof(probe)) && probe==1) pandora_ufs_probe();
#endif
 uint32_t requested=0;"""
assert t.count(old) == 1
t = t.replace(old, new)
open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0114-pandora-readonly-ufs-smmu-probe.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0114

#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'bsd/dev/sweet_ufs.c'
t = open(p).read()
diag = open('/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/pandora_ufs_diag2.c.txt').read()
anchor = "static void\npandora_ufs_gpt(void)\n"
assert t.count(anchor) == 1
t = t.replace(anchor, diag + anchor)
old = "static int\npandora_ufs_tur(void)\n"
assert t.count(old) == 1
t = t.replace(old, "static int __unused\npandora_ufs_tur(void)\n")
old = """	pandora_ufs_state("before");
	error = ufs_take_over_host();
	printf("PANDORA UFS: take over host: %d (DMA at 0x%llx)\\n", error, su.dma_pa);
	if (!error) {
		error = pandora_ufs_tur();
	}
"""
new = """	pandora_ufs_state("before");
	pandora_ufs_link("before");
	error = ufs_take_over_host();
	printf("PANDORA UFS: take over host: %d (DMA at 0x%llx)\\n", error, su.dma_pa);
	if (!error) {
		error = pandora_ufs_tur_sampled();
		pandora_ufs_state("after");
		pandora_ufs_link("after");
	}
"""
assert t.count(old) == 1, 'tur block'
t = t.replace(old, new)
open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0117-pandora-ufs-link-state-sampled-tur.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0117

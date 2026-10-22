#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'bsd/dev/sweet_ufs.c'
t = open(p).read()
diag = open('/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/pandora_ufs_diag.c.txt').read()
anchor = "static void\npandora_ufs_gpt(void)\n"
assert t.count(anchor) == 1
t = t.replace(anchor, diag + anchor)
old = """	lck_mtx_lock(&sweet_ufs_lock);
	error = ufs_take_over_host();
	printf("PANDORA UFS: take over host: %d (DMA at 0x%llx)\\n", error, su.dma_pa);
	if (!error) {
		static const uint8_t tur[6] = { 0x00 };
		/* The first command after the bootloader may report a unit attention. */
		for (int i = 0; i < 3 && (error = ufs_scsi(tur, sizeof(tur), 0, false)); i++) {
		}
		printf("PANDORA UFS: TEST UNIT READY: %d\\n", error);
	}
"""
new = """	pandora_ufs_clocks();
	lck_mtx_lock(&sweet_ufs_lock);
	pandora_ufs_state("before");
	error = ufs_take_over_host();
	printf("PANDORA UFS: take over host: %d (DMA at 0x%llx)\\n", error, su.dma_pa);
	if (!error) {
		error = pandora_ufs_tur();
	}
"""
assert t.count(old) == 1, 'tur block'
t = t.replace(old, new)
open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0116-pandora-ufs-readonly-diagnostics.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0116

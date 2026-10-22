#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'bsd/dev/sweet_ufs.c'
t = open(p).read()
h8 = open('/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/pandora_ufs_h8.c.txt').read()
anchor = "static void\npandora_ufs_gpt(void)\n"
assert t.count(anchor) == 1
t = t.replace(anchor, h8 + anchor)
old = "static int\npandora_ufs_tur_sampled(void)\n"
assert t.count(old) == 1
t = t.replace(old, "static int __unused\npandora_ufs_tur_sampled(void)\n")
old = """	if (!error) {
		error = pandora_ufs_tur_sampled();
		pandora_ufs_state("after");
		pandora_ufs_link("after");
	}
"""
new = """	if (!error) {
		error = pandora_h8_exit();
		printf("PANDORA UFS: hibernate exit: %x (HCS=%08x IS=%08x)\\n", error,
		    ufs_rd(REG_HCS), ufs_rd(REG_IS));
		pandora_ufs_link("awake");
	}
	if (!error) {
		static const uint8_t tur[6] = { 0x00 };
		/* The first command after the bootloader may report a unit attention. */
		for (int i = 0; i < 3 && (error = ufs_scsi(tur, sizeof(tur), 0, false)); i++) {
			printf("PANDORA UFS: TEST UNIT READY try %d: %d\\n", i + 1, error);
		}
		printf("PANDORA UFS: TEST UNIT READY: %d\\n", error);
		if (error) {
			pandora_ufs_state("after");
		}
	}
"""
assert t.count(old) == 1, 'tur block'
t = t.replace(old, new)
open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0118-pandora-ufs-hibern8-exit.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0118

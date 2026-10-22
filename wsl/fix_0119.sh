#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'bsd/dev/sweet_ufs.c'
t = open(p).read()
old = """		pandora_ufs_link("awake");
	}
	if (!error) {
		static const uint8_t tur[6] = { 0x00 };
"""
new = """		pandora_ufs_link("awake");
	}
	if (!error) {
		/*
		 * With the link awake LUN 0 answers TEST UNIT READY with CHECK CONDITION,
		 * sense NOT READY 04/02 ("initializing command required"): the bootloader also
		 * left the device in a low-power mode. START STOP UNIT, power condition ACTIVE,
		 * to the UFS device well-known LUN 0xD0, exactly as Linux on resume
		 * (ufshcd_set_dev_pwr_mode) and Fuxi's init; the only power command ufs_scsi's
		 * filter lets through. No data.
		 */
		static const uint8_t active[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
		su.lun = 0xd0;
		for (int i = 0; i < 3 && (error = ufs_scsi(active, sizeof(active), 0, false)); i++) {
			printf("PANDORA UFS: START STOP UNIT (ACTIVE) try %d: %d\\n", i + 1, error);
		}
		su.lun = 0;
		printf("PANDORA UFS: START STOP UNIT (ACTIVE): %d\\n", error);
	}
	if (!error) {
		static const uint8_t tur[6] = { 0x00 };
"""
assert t.count(old) == 1, 'awake block'
t = t.replace(old, new)
open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0119-pandora-ufs-device-active-power.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0119

W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    t = open(path).read()
    assert t.count(old) == 1, (path, old)
    open(path, 'w').write(t.replace(old, new))


# ---- 0247: the board thermistors, read only ---------------------------------------------------------------------
# thermadc + checks of our own: vadc@9000's thermal-monitor channels are measured by the PMIC itself, each result a
# u16 in its SDAM slot (SDAM0 0x090 slots 1-7, SDAM1 0x091 slots 0-6 in DT order). pandora_therm=1 reads them every
# second over an observer channel (new pandora_spmi_read in fuxi_keys.c, multi-byte EXT_READL), turns them into
# Android's temperatures and logs mi_thermald's body temperature every 10 s. Nothing is limited yet.
copy('pandora_therm.c', 'bsd/dev/pandora_therm.c')
copy('pandora_ntc.h', 'bsd/dev/pandora_ntc.h')
copy('pandora_ntc_table.h', 'bsd/dev/pandora_ntc_table.h')
edit('bsd/conf/files', 'bsd/dev/pandora_gpi.c\t\t\tstandard\n',
     'bsd/dev/pandora_gpi.c\t\t\tstandard\nbsd/dev/pandora_therm.c\t\t\tstandard\n')

K = 'bsd/dev/fuxi_keys.c'
edit(K, '''	*secs = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
	return 0;
}
#endif /* PANDORA */''', '''	*secs = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
	return 0;
}

/*
 * 0247: N (1-8) bytes from register REG of peripheral PPID (SID << 8 | peripheral) in one observer
 * EXT_READL, for bsd/dev/pandora_therm.c: byte count - 1 in bits 0-2, the data in RDATA0 and RDATA1, as
 * Linux spmi-pmic-arb pmic_arb_read_cmd. The arbiter is mapped as pandora_rtc_read does; the APIDs of
 * up to four PPIDs are kept. Read only. 0, or the negative step that failed.
 */
int pandora_spmi_read(uint16_t ppid, uint8_t reg, uint8_t *buf, unsigned n);
int
pandora_spmi_read(uint16_t ppid, uint8_t reg, uint8_t *buf, unsigned n)
{
	static struct {
		uint16_t ppid, apid;
	} seen[4];
	static unsigned nseen;
	if (n == 0 || n > 8) {
		return -1;
	}
	fk_core = fk_core ? fk_core : ml_io_map(PK_CORE_PA, 0x1000);
	pk_map = pk_map ? pk_map : ml_io_map(PK_MAP_PA, 0x8000);
	pk_own = pk_own ? pk_own : ml_io_map(PK_OWN_PA, 0x8000);
	fk_obs = fk_obs ? fk_obs : ml_io_map(PK_OBS_PA, 0x40000);
	if (!fk_core || !pk_map || !pk_own || !fk_obs) {
		return -2;
	}
	uint16_t apid = FK_NO_APID;
	for (unsigned i = 0; i < nseen; i++) {
		if (seen[i].ppid == ppid) {
			apid = seen[i].apid;
		}
	}
	if (apid == FK_NO_APID) {
		const uint32_t count = fk_rd(fk_core, 4) & 0x1fffu;
		unsigned owner;
		if (fk_rd(fk_core, 0) < 0x80000000u || count == 0 || count > PK_MAX_APIDS) {
			return -3;
		}
		apid = pk_find_apid(ppid, count, &owner);
		if (apid == FK_NO_APID) {
			return -3;
		}
		printf("PANDORA SPMI: PPID %#x -> APID %u (owner ee%u)\\n", ppid, apid, owner);
		if (nseen < 4) {
			seen[nseen].ppid = ppid;
			seen[nseen].apid = apid;
			nseen++;
		}
	}
	const uint32_t off = 0x20u * apid;
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(fk_obs + off) = (1u << 27) | ((uint32_t)reg << 4) | (n - 1u);
	__asm__ volatile("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000; ++i) {
		const uint32_t st = fk_rd(fk_obs, off + 0x08u);
		if (st & 1u) {
			if (st & 0x0eu) {
				return -4;
			}
			const uint32_t d[2] = {fk_rd(fk_obs, off + 0x18u), n > 4 ? fk_rd(fk_obs, off + 0x1cu) : 0u};
			for (unsigned k = 0; k < n; k++) {
				buf[k] = (uint8_t)(d[k / 4] >> (8 * (k % 4)));
			}
			return 0;
		}
		delay(1);
	}
	return -5;
}
#endif /* PANDORA */''')

U = 'bsd/dev/sweet_ufs.c'
edit(U, '''extern void pandora_cpu_init(void);
#endif''', '''extern void pandora_cpu_init(void);
extern void pandora_therm_init(void);
#endif''')
edit(U, '''	/* CPU performance probe, boot-arg pandora_cpu (bsd/dev/pandora_cpu.c). */
	pandora_cpu_init();
#endif''', '''	/* CPU performance probe, boot-arg pandora_cpu (bsd/dev/pandora_cpu.c). */
	pandora_cpu_init();
	/* Board thermistors, read only, boot-arg pandora_therm (bsd/dev/pandora_therm.c). */
	pandora_therm_init();
#endif''')
print('patched (board thermistors, read only)')

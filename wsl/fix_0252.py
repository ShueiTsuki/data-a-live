W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    t = open(path).read()
    assert t.count(old) == 1, (path, old)
    open(path, 'w').write(t.replace(old, new))


# ---- 0252: the PMIC measures the board thermistors again ---------------------------------------------------------
# bat4: under iOS every thermal-monitor slot of vadc@9000 is 0; the PMIC no longer measures them (Android's driver set
# them up, the restart lost it). pandora_therm=2 sets the 14 channels up the way Linux qcom-spmi-adc-tm5-gen3
# adc_tm5_gen3_configure does (1 s period, no threshold interrupts), which needs SPMI writes: new pandora_spmi_write
# in fuxi_keys.c, an EXT_WRITEL on the PPID's read-write channel (arbiter v8 "chnls"), only where EE 0 owns the
# PPID. pandora_spmi_read and it share the APID lookup. pandora_therm=1 stays read only.
copy('pandora_therm.c', 'bsd/dev/pandora_therm.c')

K = 'bsd/dev/fuxi_keys.c'
edit(K, '''#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
''', '''#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#if defined(PANDORA)
#include <vm/pmap.h>
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
#endif
''')
edit(K, '''int pandora_spmi_read(uint16_t ppid, uint8_t reg, uint8_t *buf, unsigned n);
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
	const uint32_t off = 0x20u * apid;''', '''static struct {
	uint16_t ppid, apid;
	uint8_t owner;
} ps_seen[4];
static unsigned ps_nseen;

/* The arbiter mapped, and the APID of PPID with its write owner (EE); the last four are kept. 0, or the
 * negative step that failed. */
static int
ps_apid(uint16_t ppid, uint16_t *apid_out, unsigned *owner_out)
{
	fk_core = fk_core ? fk_core : ml_io_map(PK_CORE_PA, 0x1000);
	pk_map = pk_map ? pk_map : ml_io_map(PK_MAP_PA, 0x8000);
	pk_own = pk_own ? pk_own : ml_io_map(PK_OWN_PA, 0x8000);
	fk_obs = fk_obs ? fk_obs : ml_io_map(PK_OBS_PA, 0x40000);
	if (!fk_core || !pk_map || !pk_own || !fk_obs) {
		return -2;
	}
	for (unsigned i = 0; i < ps_nseen; i++) {
		if (ps_seen[i].ppid == ppid) {
			*apid_out = ps_seen[i].apid;
			*owner_out = ps_seen[i].owner;
			return 0;
		}
	}
	const uint32_t count = fk_rd(fk_core, 4) & 0x1fffu;
	unsigned owner;
	if (fk_rd(fk_core, 0) < 0x80000000u || count == 0 || count > PK_MAX_APIDS) {
		return -3;
	}
	const uint16_t apid = pk_find_apid(ppid, count, &owner);
	if (apid == FK_NO_APID) {
		return -3;
	}
	printf("PANDORA SPMI: PPID %#x -> APID %u (owner ee%u)\\n", ppid, apid, owner);
	if (ps_nseen < 4) {
		ps_seen[ps_nseen].ppid = ppid;
		ps_seen[ps_nseen].apid = apid;
		ps_seen[ps_nseen].owner = (uint8_t)owner;
		ps_nseen++;
	}
	*apid_out = apid;
	*owner_out = owner;
	return 0;
}

int pandora_spmi_read(uint16_t ppid, uint8_t reg, uint8_t *buf, unsigned n);
int
pandora_spmi_read(uint16_t ppid, uint8_t reg, uint8_t *buf, unsigned n)
{
	if (n == 0 || n > 8) {
		return -1;
	}
	uint16_t apid;
	unsigned owner;
	const int e = ps_apid(ppid, &apid, &owner);
	if (e) {
		return e;
	}
	const uint32_t off = 0x20u * apid;''')
edit(K, '''		delay(1);
	}
	return -5;
}
#endif /* PANDORA */''', '''		delay(1);
	}
	return -5;
}

/*
 * 0252: N (1-8) bytes to register REG of peripheral PPID in one EXT_WRITEL (opcode 0) on PPID's read-write
 * channel, as Linux spmi-pmic-arb pmic_arb_write_cmd on v8: "chnls" 0x0c900000 + 0x200 * APID, the data in
 * WDATA0 (+0x10) and WDATA1 (+0x14), then the command at +0, byte count - 1 in bits 0-2; STATUS (+0x08)
 * bit 0 done, bits 1-6 failed, CRC, parity, NACK, denied, dropped (pmic_arb_check_chnl_status_v8p5).
 * Only for a PPID whose write owner is EE 0 (ours), as Linux checks. For pandora_therm.c's thermal-monitor
 * setup. 0, or the negative step that failed (-6 not ours to write).
 */
#define PS_CHNLS_PA 0x0c900000u

int pandora_spmi_write(uint16_t ppid, uint8_t reg, const uint8_t *buf, unsigned n);
int
pandora_spmi_write(uint16_t ppid, uint8_t reg, const uint8_t *buf, unsigned n)
{
	static struct {
		uint32_t pa;
		vm_offset_t va;
	} page[2];
	if (n == 0 || n > 8) {
		return -1;
	}
	uint16_t apid;
	unsigned owner;
	const int e = ps_apid(ppid, &apid, &owner);
	if (e) {
		return e;
	}
	if (owner != 0) {
		return -6;
	}
	const uint32_t pa = PS_CHNLS_PA + 0x200u * apid;
	vm_offset_t va = 0;
	for (unsigned i = 0; i < 2 && !va; i++) {
		if (!page[i].va) {
			page[i].va = ml_io_map_unmappable(pa & ~0xfffu, 0x1000, VM_WIMG_IO);
			page[i].pa = pa & ~0xfffu;
		}
		if (page[i].va && page[i].pa == (pa & ~0xfffu)) {
			va = page[i].va;
		}
	}
	if (!va) {
		return -2;
	}
	const vm_offset_t ch = va + (pa & 0xfffu);
	uint32_t d[2] = {0, 0};
	for (unsigned k = 0; k < n; k++) {
		d[k / 4] |= (uint32_t)buf[k] << (8 * (k % 4));
	}
	*(volatile uint32_t *)(ch + 0x10u) = d[0];
	if (n > 4) {
		*(volatile uint32_t *)(ch + 0x14u) = d[1];
	}
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(ch + 0x00u) = ((uint32_t)reg << 4) | (n - 1u);
	__asm__ volatile("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000; ++i) {
		const uint32_t st = fk_rd(ch, 0x08u);
		if (st & 1u) {
			return (st & 0x7eu) ? -4 : 0;
		}
		delay(1);
	}
	return -5;
}
#endif /* PANDORA */''')
print('patched (thermal-monitor setup, SPMI writes)')

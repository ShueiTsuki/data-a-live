W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def edit(path, old, new):
    t = open(path).read()
    assert t.count(old) == 1, (path, old)
    open(path, 'w').write(t.replace(old, new))


# ---- 0255: the thermistors' PMIC GPIOs ------------------------------------------------------------------------
# usb6: decimation is not why SDAM0 slots 1-6 read 20-26 degrees hot (2 and 1 within 1 degree in one boot). Those six
# channels (and cpu_therm) go through PMIC GPIOs that Android's pinctrl puts in high impedance; nobody does under
# iOS. pandora_therm=2 now dumps the seven GPIOs' registers, and with pandora_therm_gpio=1 clears their EN_CTL (as
# Linux pinctrl-spmi-gpio does for bias-high-impedance) between two code dumps. Two of them are on pmih010x on SPMI bus 1, so ps_apid in
# fuxi_keys.c learns arbiter v8's bus 1: its APIDs follow bus 0's, its owner table is spmi@c437000's chnl_owner.
copy('pandora_therm.c', 'bsd/dev/pandora_therm.c')

K = 'bsd/dev/fuxi_keys.c'
edit(K, '''static struct {
	uint16_t ppid, apid;
	uint8_t owner;
} ps_seen[4];
static unsigned ps_nseen;

/* The arbiter mapped, and the APID of PPID with its write owner (EE); the last four are kept. 0, or the
 * negative step that failed. */
static int
ps_apid(uint16_t ppid, uint16_t *apid_out, unsigned *owner_out)
{''', '''static struct {
	uint16_t ppid, apid;
	uint8_t owner;
} ps_seen[16];
static unsigned ps_nseen;

#define PS_OWN1_PA 0x0c43b000u   /* bus 1's "chnl_owner" (spmi@c437000) */
static vm_offset_t ps_own1;

/* The arbiter mapped, and the APID of PPID with its write owner (EE); sixteen are kept. 0, or the
 * negative step that failed. PPID is the SID byte << 8 | peripheral, the SID byte with the bus in bits
 * 5-6 as the ADC writes it (pmih010x 0x27: bus 1, SID 7). Arbiter v8 numbers bus 1's APIDs after bus
 * 0's (FEATURES +4 and +8: each bus's count), keeps one APID->PPID map for both and an owner table per
 * bus, indexed from the bus's first APID (Linux _pmic_arb_init_apid_v7, pmic_arb_apid_owner_v8). */
static int
ps_apid(uint16_t ppid, uint16_t *apid_out, unsigned *owner_out)
{''')
edit(K, '''	const uint32_t count = fk_rd(fk_core, 4) & 0x1fffu;
	unsigned owner;
	if (fk_rd(fk_core, 0) < 0x80000000u || count == 0 || count > PK_MAX_APIDS) {
		return -3;
	}
	const uint16_t apid = pk_find_apid(ppid, count, &owner);
	if (apid == FK_NO_APID) {
		return -3;
	}
	printf("PANDORA SPMI: PPID %#x -> APID %u (owner ee%u)\\n", ppid, apid, owner);
	if (ps_nseen < 4) {''', '''	const unsigned bus = ppid >> 13;
	if (bus > 1) {
		return -3;
	}
	uint32_t base = 0, count = 0;
	for (unsigned b = 0; b <= bus; b++) {
		base += count;
		count = fk_rd(fk_core, 4 + 4 * b) & 0x1fffu;
	}
	if (fk_rd(fk_core, 0) < 0x80000000u || count == 0 || base + count > PK_MAX_APIDS) {
		return -3;
	}
	if (bus && !ps_own1) {
		ps_own1 = ml_io_map_unmappable(PS_OWN1_PA, 0x8000, VM_WIMG_IO);
	}
	const vm_offset_t own = bus ? ps_own1 : pk_own;
	if (!own) {
		return -2;
	}
	uint16_t apid = FK_NO_APID;
	unsigned owner = 7;
	for (uint32_t i = base; i < base + count; ++i) {
		const uint32_t map = fk_rd(pk_map, 4u * i);
		if (map == 0 || (map & 0x1fffu) != (ppid & 0x1fffu)) {
			continue;
		}
		const unsigned o = fk_rd(own, 4u * (i - base)) & 7u;
		if (apid == FK_NO_APID || o == 0) {
			apid = (uint16_t)i;
			owner = o;
		}
	}
	if (apid == FK_NO_APID) {
		return -3;
	}
	printf("PANDORA SPMI: PPID %#x (bus %u) -> APID %u (owner ee%u)\\n", ppid & 0x1fffu, bus, apid, owner);
	if (ps_nseen < 16) {''')
print('patched (thermistor GPIOs, SPMI bus 1)')

/*
 * PANDORA audio DSP (ADSP) bring-up. Boot-arg pandora_adsp: 1 = TrustZone queries only,
 * 2 = also load and start the ADSP (pandora_adsp_delay seconds after start, default 20).
 *
 * TrustZone (SCM) calls as Linux drivers/firmware/qcom/qcom_scm-smc.c: SMCCC yielding
 * call, SMC64, owner SIP, function (svc << 8) | cmd, x1 = arginfo (count | 2-bit types),
 * x2..x5 arguments; x0 = 1 (interrupted) means call again with x0 = 1 and the returned x6.
 *
 * Start sequence as drivers/remoteproc/qcom_q6v5_pas.c (canoe: pas 1, DTB image pas 36,
 * like "nord_adsp_resource"/sm8550): AOP load_state "adsp" on (QMP), RPMh LCX and LMX
 * ARC levels up (DT cx/mx-supply "lcx.lvl"/"lmx.lvl", 384), then for the DTB image and
 * then the main image: metadata (ELF header + program headers, then the hash segment,
 * qcom_mdt_read_metadata) in memory registered with an SHM bridge, PAS_INIT_IMAGE,
 * PAS_MEM_SETUP when relocatable, segments copied to the carveout (relocated to the
 * lowest segment address, qcom_mdt_load_no_init), PAS_AUTH_AND_RESET. Carveouts from the
 * DT: adspslpi 0xa1400000 + 0x5580000, q6_adsp_dtb 0xa1380000 + 0x80000. Firmware comes
 * from the pinned firmware pack (tools/mkfwpack.py, pandora_fw_read in sweet_ufs.c).
 * Not done yet: interconnect votes, SMP2P ready/handover, GLINK. PLAN.md "扬声器路线".
 */
#if defined(PANDORA)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <arm/caches_internal.h>

extern kern_return_t thread_terminate(thread_t);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern int pandora_fw_read(uint64_t off, void *dst, uint64_t len);
void pandora_adsp_init(void);

#define AD_PAS_MAIN      1u
#define AD_PAS_DTB       36u
#define AD_MEM_PA        0xa1400000ull
#define AD_MEM_SIZE      0x5580000ull
#define AD_DTB_PA        0xa1380000ull
#define AD_DTB_SIZE      0x80000ull
#define AD_QMP_PA        0x0c300000u
#define AD_IPCC_PA       0x01106000u
#define AD_RSC_PA        0x18920000u
#define AD_CMDDB_PA      0x81c60000u
#define AD_CMDDB_SIZE    0x20000u

static int ad_chan;

static void
ad_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&ad_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static uint32_t
ad_rd(vm_offset_t b, uint32_t o)
{
	uint32_t v = *(volatile uint32_t *)(b + o);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static void
ad_wr(vm_offset_t b, uint32_t o, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(b + o) = v;
	__asm__ volatile ("dsb osh" ::: "memory");
}

/* ---- SCM ------------------------------------------------------------------------ */

struct scm_res {
	uint64_t a0, a1, a2, a3;
};

static void
scm_smc(uint64_t fn, uint64_t arginfo, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5,
    struct scm_res *res)
{
	uint64_t a0 = fn, a6 = 0;
	for (int tries = 0; tries < 100000; tries++) {
		register uint64_t r0 __asm__("x0") = a0;
		register uint64_t r1 __asm__("x1") = arginfo;
		register uint64_t r2 __asm__("x2") = x2;
		register uint64_t r3 __asm__("x3") = x3;
		register uint64_t r4 __asm__("x4") = x4;
		register uint64_t r5 __asm__("x5") = x5;
		register uint64_t r6 __asm__("x6") = a6;
		register uint64_t r7 __asm__("x7") = 0;
		__asm__ volatile ("smc #0"
		    : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6), "+r"(r7)
		    :
		    : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		res->a0 = r0;
		res->a1 = r1;
		res->a2 = r2;
		res->a3 = r3;
		if (r0 != 1) {           /* QCOM_SCM_INTERRUPTED */
			return;
		}
		a0 = 1;
		a6 = r6;
	}
}

#define SCM_FN(svc, cmd)  ((1ULL << 30) | (2ULL << 24) | (((uint64_t)(svc) & 0xff) << 8) | ((cmd) & 0xff))
#define SCM_ARGS(n)       ((uint64_t)(n) & 0xf)
#define SCM_RW            2u

static int
scm_call_avail(unsigned svc, unsigned cmd)
{
	struct scm_res r;
	uint64_t id = (2ULL << 24) | ((svc & 0xff) << 8) | (cmd & 0xff);
	scm_smc(SCM_FN(6, 1), SCM_ARGS(1), id, 0, 0, 0, &r);
	printf("PANDORA ADSP: is_call_avail svc %#x cmd %#x -> a0 %#llx a1 %#llx\n", svc, cmd, r.a0, r.a1);
	return r.a0 == 0 ? (int)r.a1 : -1;
}

/* ---- AOP QMP (drivers/soc/qcom/qcom_aoss.c), doorbell IPCC client 0 signal 0 ------ */

static vm_offset_t ad_qmp, ad_ipcc;

static void
qmp_kick(void)
{
	ad_wr(ad_ipcc, 0x0c, 0);         /* IPCC_REG_SEND_ID: client 0 (AOP) << 16 | signal 0 */
}

static bool
qmp_wait(uint32_t off, uint32_t want)
{
	for (int i = 0; i < 1000; i++) {
		if (ad_rd(ad_qmp, off) == want) {
			return true;
		}
		ad_sleep(1);
	}
	return false;
}

static int
qmp_send(const char *msg)
{
	if (!ad_qmp) {
		ad_qmp = ml_io_map(AD_QMP_PA, 0x400);
		ad_ipcc = ml_io_map(AD_IPCC_PA, 0x1000);
		if (!ad_qmp || !ad_ipcc) {
			return ENOMEM;
		}
		uint32_t magic = ad_rd(ad_qmp, 0x0), ver = ad_rd(ad_qmp, 0x4);
		printf("PANDORA ADSP: QMP magic %#x version %u mcore link %#x ch %#x ucore link %#x ch %#x\n",
		    magic, ver, ad_rd(ad_qmp, 0x24), ad_rd(ad_qmp, 0x2c), ad_rd(ad_qmp, 0xc), ad_rd(ad_qmp, 0x14));
		if (magic != 0x4d41494c || ver != 1) {
			return ENODEV;
		}
		ad_wr(ad_qmp, 0x10, ad_rd(ad_qmp, 0x0c));       /* ack ucore link */
		ad_wr(ad_qmp, 0x24, 0xffff);                    /* mcore link up */
		qmp_kick();
		bool l = qmp_wait(0x28, 0xffff);
		ad_wr(ad_qmp, 0x2c, 0xffff);                    /* mcore channel up */
		qmp_kick();
		bool u = qmp_wait(0x14, 0xffff);
		ad_wr(ad_qmp, 0x18, 0xffff);                    /* ack ucore channel */
		qmp_kick();
		bool c = qmp_wait(0x30, 0xffff);
		printf("PANDORA ADSP: QMP open: link ack %d ucore ch %d ch ack %d\n", l, u, c);
		if (!l || !u || !c) {
			return ETIMEDOUT;
		}
	}
	uint32_t off = ad_rd(ad_qmp, 0x38), size = ad_rd(ad_qmp, 0x34);
	if (size < 68 || off + 68 > 0x400) {
		return EINVAL;
	}
	char buf[64];
	bzero(buf, sizeof(buf));
	strlcpy(buf, msg, sizeof(buf));
	for (unsigned i = 0; i < 16; i++) {
		uint32_t w;
		memcpy(&w, buf + 4 * i, 4);
		ad_wr(ad_qmp, off + 4 + 4 * i, w);
	}
	ad_wr(ad_qmp, off, 64);
	(void)ad_rd(ad_qmp, off);
	qmp_kick();
	bool done = qmp_wait(off, 0);
	printf("PANDORA ADSP: QMP \"%s\" -> %s\n", msg, done ? "taken" : "TIMEOUT");
	if (!done) {
		ad_wr(ad_qmp, off, 0);
		return ETIMEDOUT;
	}
	return 0;
}

/* ---- RPMh ARC level vote (apps RSC DRV 2, v3 layout, cmd-db) ---------------------- */

static vm_offset_t ad_rsc, ad_db;

static uint16_t
db16(uint32_t off)
{
	return (uint16_t)(ad_rd(ad_db, off & ~3u) >> (8u * (off & 3u)));
}

/* cmd-db entry: address, aux data offset and length. */
static int
cmddb_find(const char *res, uint32_t *addr, uint32_t *aux, uint32_t *auxlen)
{
	uint32_t key[2] = {0, 0};
	for (unsigned i = 0; i < 8 && res[i]; i++) {
		key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8u * (i % 4));
	}
	for (unsigned i = 0; i < 8; i++) {
		uint32_t h = 8u + 16u * i;
		if (!db16(h)) {
			break;
		}
		uint32_t entries = 144 + db16(h + 2), data = 144 + db16(h + 4);
		unsigned count = db16(h + 6);
		for (unsigned j = 0; j < count && entries + 24 * (j + 1) <= AD_CMDDB_SIZE; j++) {
			uint32_t e = entries + 24 * j;
			if (ad_rd(ad_db, e) == key[0] && ad_rd(ad_db, e + 4) == key[1]) {
				*addr = ad_rd(ad_db, e + 16);
				*auxlen = db16(e + 20);
				*aux = data + db16(e + 22);
				return 0;
			}
		}
	}
	return ENOENT;
}

static int
rpmh_write1(uint32_t addr, uint32_t data)
{
	const uint32_t tcs = 0xd00, stride = 0x2a0;
	int chosen = -1;
	for (unsigned t = 0; t < 3 && chosen < 0; t++) {
		uint32_t b = tcs + t * stride;
		if (ad_rd(ad_rsc, b + 0x28) != 0 && ad_rd(ad_rsc, b + 0x24) == 0 &&
		    ad_rd(ad_rsc, b + 0x2c) == 0 && ad_rd(ad_rsc, b + 0x20) == 0) {
			chosen = (int)t;
		}
	}
	if (chosen < 0) {
		return EBUSY;
	}
	uint32_t b = tcs + (uint32_t)chosen * stride, bit = 1u << chosen;
	uint32_t irq_en = ad_rd(ad_rsc, tcs + 0);
	ad_wr(ad_rsc, tcs + 0, irq_en & ~bit);
	ad_wr(ad_rsc, b + 0x34, 8u | (1u << 8) | (1u << 16));   /* msgid: write, response */
	ad_wr(ad_rsc, b + 0x38, addr);
	ad_wr(ad_rsc, b + 0x3c, data);
	ad_wr(ad_rsc, b + 0x20, 1);      /* wait for completion */
	ad_wr(ad_rsc, b + 0x2c, 1);      /* command 0 enabled */
	ad_wr(ad_rsc, b + 0x24, 0);
	ad_wr(ad_rsc, b + 0x24, 1u << 16);
	ad_wr(ad_rsc, b + 0x24, (1u << 16) | (1u << 24));
	bool done = false;
	for (int us = 0; us < 100000 && !done; us++) {
		uint32_t cs = ad_rd(ad_rsc, b + 0x40);
		done = (cs & 0x10100u) == 0x10100u && ad_rd(ad_rsc, b + 0x28) != 0;
		if (!done) {
			delay(1);
		}
	}
	ad_wr(ad_rsc, b + 0x24, 1u << 16);
	ad_wr(ad_rsc, b + 0x24, 0);
	ad_wr(ad_rsc, b + 0x2c, 0);
	ad_wr(ad_rsc, b + 0x20, 0);
	ad_wr(ad_rsc, tcs + 8, bit);
	ad_wr(ad_rsc, tcs + 0, irq_en);
	return done ? 0 : ETIMEDOUT;
}

/* ARC: the vote is the index of the first level in the aux table at or above vlvl. */
static int
arc_vote(const char *res, uint32_t vlvl)
{
	uint32_t addr = 0, aux = 0, len = 0;
	int err = cmddb_find(res, &addr, &aux, &len);
	if (err) {
		printf("PANDORA ADSP: cmd-db %s not found\n", res);
		return err;
	}
	uint32_t hlvl = 0, n = len / 2, got = 0;
	for (; hlvl < n; hlvl++) {
		got = db16(aux + 2 * hlvl);
		if (got >= vlvl) {
			break;
		}
	}
	if (hlvl == n) {
		hlvl = n - 1;
	}
	err = rpmh_write1(addr, hlvl);
	printf("PANDORA ADSP: ARC %s addr %#x levels %u: vote hlvl %u (level %u) -> %d\n", res, addr, n,
	    hlvl, got, err);
	return err;
}

/* ---- firmware pack and MDT --------------------------------------------------------- */

struct fw_ent {
	char name[48];
	uint64_t off, size;
};
static struct fw_ent ad_dir[62];
static unsigned ad_nfiles;

static int
fw_dir(void)
{
	uint8_t blk[4096];
	int err = pandora_fw_read(0, blk, sizeof(blk));
	if (err) {
		printf("PANDORA ADSP: firmware pack read failed %d (no --fw file?)\n", err);
		return err;
	}
	uint32_t n;
	memcpy(&n, blk + 8, 4);
	if (memcmp(blk, "PANDFW01", 8) || n == 0 || n > 62) {
		printf("PANDORA ADSP: no firmware pack header\n");
		return EINVAL;
	}
	memcpy(ad_dir, blk + 16, n * sizeof(struct fw_ent));
	ad_nfiles = n;
	printf("PANDORA ADSP: firmware pack: %u files\n", n);
	return 0;
}

static struct fw_ent *
fw_find(const char *name)
{
	for (unsigned i = 0; i < ad_nfiles; i++) {
		ad_dir[i].name[47] = 0;
		if (!strcmp(ad_dir[i].name, name)) {
			return &ad_dir[i];
		}
	}
	return NULL;
}

struct elf32_phdr {
	uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
};
#define MDT_TYPE_MASK    (7u << 24)
#define MDT_TYPE_HASH    (2u << 24)
#define MDT_RELOCATABLE  (1u << 27)

static bool
mdt_loadable(const struct elf32_phdr *p)
{
	return p->p_type == 1 && (p->p_flags & MDT_TYPE_MASK) != MDT_TYPE_HASH && p->p_memsz;
}

/* Read segment i of base.mdt: from base.bNN (split images) or from the .mdt itself. */
static int
mdt_segment(const char *base, const struct fw_ent *mdt, const struct elf32_phdr *p, unsigned i,
    void *dst)
{
	char name[48];
	snprintf(name, sizeof(name), "%s.b%02u", base, i);
	struct fw_ent *seg = fw_find(name);
	if (seg) {
		if (seg->size != p->p_filesz) {
			printf("PANDORA ADSP: %s is %llu bytes, segment wants %u\n", name, seg->size, p->p_filesz);
			return EINVAL;
		}
		return pandora_fw_read(seg->off, dst, p->p_filesz);
	}
	if ((uint64_t)p->p_offset + p->p_filesz <= mdt->size) {
		return pandora_fw_read(mdt->off + p->p_offset, dst, p->p_filesz);
	}
	printf("PANDORA ADSP: segment %u of %s missing\n", i, base);
	return ENOENT;
}

/*
 * One image: metadata to TrustZone through an SHM bridge, init, mem setup, segments into
 * the carveout. The bridge handle is returned for deletion after the reset.
 */
static int
pas_load(const char *base, unsigned pas, uint64_t mem_pa, uint64_t mem_size, uint64_t *bridge)
{
	char name[48];
	snprintf(name, sizeof(name), "%s.mdt", base);
	struct fw_ent *mdt = fw_find(name);
	if (!mdt || mdt->size < 52 || mdt->size > 0x10000) {
		printf("PANDORA ADSP: %s missing\n", name);
		return ENOENT;
	}
	static uint8_t hdr[0x10000];
	int err = pandora_fw_read(mdt->off, hdr, mdt->size);
	if (err) {
		return err;
	}
	uint32_t phoff, phnum;
	memcpy(&phoff, hdr + 28, 4);
	phnum = hdr[44] | (hdr[45] << 8);
	if (memcmp(hdr, "\177ELF", 4) || hdr[4] != 1 || phnum < 2 || phnum > 128 ||
	    phoff + phnum * sizeof(struct elf32_phdr) > mdt->size) {
		printf("PANDORA ADSP: %s is not a 32-bit ELF header\n", name);
		return EINVAL;
	}
	const struct elf32_phdr *ph = (const struct elf32_phdr *)(hdr + phoff);
	unsigned hash = 0;
	uint64_t min = ~0ull, max = 0;
	bool reloc = false;
	for (unsigned i = 0; i < phnum; i++) {
		if (!hash && i && (ph[i].p_flags & MDT_TYPE_MASK) == MDT_TYPE_HASH) {
			hash = i;
		}
		if (!mdt_loadable(&ph[i])) {
			continue;
		}
		reloc |= (ph[i].p_flags & MDT_RELOCATABLE) != 0;
		min = MIN(min, (uint64_t)ph[i].p_paddr);
		max = MAX(max, ((uint64_t)ph[i].p_paddr + ph[i].p_memsz + 0xfff) & ~0xfffull);
	}
	if (!hash || ph[0].p_type == 1 || min == ~0ull) {
		printf("PANDORA ADSP: %s: no hash segment\n", name);
		return EINVAL;
	}
	uint32_t ehdr_size = ph[0].p_filesz, hash_size = ph[hash].p_filesz;
	uint64_t md_size = ((uint64_t)ehdr_size + hash_size + 0xfff) & ~0xfffull;
	printf("PANDORA ADSP: %s: %u segments, hash %u (%u bytes), load %#llx-%#llx%s\n", name, phnum,
	    hash, hash_size, min, max, reloc ? " relocatable" : "");
	if (max - min > mem_size) {
		printf("PANDORA ADSP: %s does not fit its carveout\n", name);
		return EINVAL;
	}
	vm_offset_t md = 0;
	if (kmem_alloc_contig(kernel_map, &md, (vm_size_t)md_size, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return ENOMEM;
	}
	uint64_t md_pa = ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)md));
	memcpy((void *)md, hdr, ehdr_size);
	if (ehdr_size + hash_size == mdt->size) {
		memcpy((void *)(md + ehdr_size), hdr + ehdr_size, hash_size);
	} else if ((uint64_t)ph[hash].p_offset + hash_size <= mdt->size) {
		memcpy((void *)(md + ehdr_size), hdr + ph[hash].p_offset, hash_size);
	} else if ((err = mdt_segment(base, mdt, &ph[hash], hash, (void *)(md + ehdr_size)))) {
		return err;
	}
	flush_dcache64((addr64_t)md_pa, (unsigned)md_size, TRUE);
	struct scm_res r;
	scm_smc(SCM_FN(0xc, 0x1e), SCM_ARGS(4), md_pa | 6, md_pa | 6, md_size | (1ull << 9), 3, &r);
	printf("PANDORA ADSP: shm bridge %#llx+%#llx -> a0 %#llx a1 %#llx handle %#llx\n", md_pa, md_size,
	    r.a0, r.a1, r.a2);
	if (r.a0 || r.a1) {
		return EIO;
	}
	*bridge = r.a2;
	scm_smc(SCM_FN(2, 1), SCM_ARGS(2) | (SCM_RW << 6), pas, md_pa, 0, 0, &r);
	printf("PANDORA ADSP: pas %u init_image (%u bytes) -> a0 %#llx (%d) a1 %#llx\n", pas,
	    ehdr_size + hash_size, r.a0, (int)(int32_t)r.a0, r.a1);
	if (r.a0 || r.a1) {
		return EIO;
	}
	if (reloc) {
		scm_smc(SCM_FN(2, 2), SCM_ARGS(3), pas, mem_pa, max - min, 0, &r);
		printf("PANDORA ADSP: pas %u mem_setup %#llx+%#llx -> a0 %#llx a1 %#llx\n", pas, mem_pa,
		    max - min, r.a0, r.a1);
		if (r.a0 || r.a1) {
			return EIO;
		}
	}
	vm_offset_t mem = ml_io_map_wcomb((vm_offset_t)mem_pa, (vm_size_t)mem_size);
	if (!mem) {
		return ENOMEM;
	}
	uint64_t reloc_base = reloc ? min : mem_pa, total = 0;
	for (unsigned i = 0; i < phnum; i++) {
		if (!mdt_loadable(&ph[i])) {
			continue;
		}
		uint64_t off = (uint64_t)ph[i].p_paddr - reloc_base;
		if ((int64_t)off < 0 || off + ph[i].p_memsz > mem_size || ph[i].p_filesz > ph[i].p_memsz) {
			printf("PANDORA ADSP: %s segment %u outside the carveout\n", name, i);
			return EINVAL;
		}
		if (ph[i].p_filesz && (err = mdt_segment(base, mdt, &ph[i], i, (void *)(mem + off)))) {
			return err;
		}
		if (ph[i].p_memsz > ph[i].p_filesz) {
			bzero((void *)(mem + off + ph[i].p_filesz), ph[i].p_memsz - ph[i].p_filesz);
		}
		total += ph[i].p_filesz;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("PANDORA ADSP: %s: %llu bytes loaded at %#llx\n", name, total, mem_pa);
	return 0;
}

static int
pas_auth(unsigned pas)
{
	struct scm_res r;
	uint64_t t0 = mach_absolute_time(), ns;
	scm_smc(SCM_FN(2, 5), SCM_ARGS(1), pas, 0, 0, 0, &r);
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	printf("PANDORA ADSP: pas %u auth_and_reset -> a0 %#llx a1 %#llx (%llu ms)\n", pas, r.a0, r.a1,
	    ns / 1000000);
	return (r.a0 || r.a1) ? EIO : 0;
}

static void
adsp_boot(void)
{
	int err;
	uint64_t b_dtb = 0, b_main = 0;
	struct scm_res r;
	if ((err = fw_dir())) {
		return;
	}
	ad_rsc = ml_io_map(AD_RSC_PA, 0x10000);
	ad_db = ml_io_map(AD_CMDDB_PA, AD_CMDDB_SIZE);
	if (!ad_rsc || !ad_db) {
		return;
	}
	(void)arc_vote("lcx.lvl", 384);
	(void)arc_vote("lmx.lvl", 384);
	if ((err = qmp_send("{class: image, res: load_state, name: adsp, val: on}"))) {
		printf("PANDORA ADSP: load_state failed %d\n", err);
	}
	/* Images the boot loaders may have left running (x1e80100: "lite" ADSP pas 0x1f and
	 * DTB 0x29), and our own ids: shut down first, as qcom_pas_load does for lite ids. */
	static const unsigned stop[] = {0x1f, 0x29, AD_PAS_MAIN, AD_PAS_DTB};
	for (unsigned i = 0; i < 4; i++) {
		struct scm_res s, q;
		scm_smc(SCM_FN(2, 7), SCM_ARGS(1), stop[i], 0, 0, 0, &q);
		scm_smc(SCM_FN(2, 6), SCM_ARGS(1), stop[i], 0, 0, 0, &s);
		printf("PANDORA ADSP: pas %#x supported a0 %#llx a1 %#llx, shutdown -> a0 %#llx a1 %#llx\n",
		    stop[i], q.a0, q.a1, s.a0, s.a1);
	}
	if ((err = pas_load("adsp_dtb", AD_PAS_DTB, AD_DTB_PA, AD_DTB_SIZE, &b_dtb)) ||
	    (err = pas_auth(AD_PAS_DTB))) {
		/* Go on with the main image anyway: its answer tells more. */
		printf("PANDORA ADSP: DTB image failed %d\n", err);
	}
	if ((err = pas_load("adsp", AD_PAS_MAIN, AD_MEM_PA, AD_MEM_SIZE, &b_main)) ||
	    (err = pas_auth(AD_PAS_MAIN))) {
		printf("PANDORA ADSP: main image failed %d\n", err);
		return;
	}
	printf("PANDORA ADSP: ADSP released from reset\n");
	ad_sleep(2000);
	scm_smc(SCM_FN(0xc, 0x1d), SCM_ARGS(1), b_main, 0, 0, 0, &r);
	scm_smc(SCM_FN(0xc, 0x1d), SCM_ARGS(1), b_dtb, 0, 0, 0, &r);
}

static void
adsp_thread(void *arg, wait_result_t result)
{
	struct scm_res r;
	int mode = 0;
	unsigned delay_s = 20;
	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("pandora_adsp", &mode, sizeof(mode));
	(void)PE_parse_boot_argn("pandora_adsp_delay", &delay_s, sizeof(delay_s));
	printf("PANDORA ADSP: SCM probe\n");
	(void)scm_call_avail(2, 1);
	(void)scm_call_avail(2, 5);
	(void)scm_call_avail(0xc, 0x1e);
	static const unsigned pas[] = {AD_PAS_MAIN, AD_PAS_DTB};
	for (unsigned i = 0; i < 2; i++) {
		scm_smc(SCM_FN(2, 7), SCM_ARGS(1), pas[i], 0, 0, 0, &r);
		printf("PANDORA ADSP: pas_is_supported(%u) -> a0 %#llx a1 %#llx\n", pas[i], r.a0, r.a1);
	}
	scm_smc(SCM_FN(0xc, 0x1c), SCM_ARGS(0), 0, 0, 0, 0, &r);
	printf("PANDORA ADSP: shm_bridge_enable -> a0 %#llx a1 %#llx\n", r.a0, r.a1);
	if (mode >= 2) {
		ad_sleep(delay_s * 1000u);
		adsp_boot();
	}
	printf("PANDORA ADSP: done\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
pandora_adsp_init(void)
{
	thread_t thread;
	int on = 0;
	(void)PE_parse_boot_argn("pandora_adsp", &on, sizeof(on));
	if (!on) {
		return;
	}
	if (kernel_thread_start(adsp_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */

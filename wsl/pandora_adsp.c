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
 * qcom_mdt_read_metadata) in plain contiguous memory (the hypervisor bridges it), PAS_INIT_IMAGE,
 * PAS_MEM_SETUP when relocatable, segments copied to the carveout (relocated to the
 * lowest segment address, qcom_mdt_load_no_init), PAS_AUTH_AND_RESET. Carveouts from the
 * DT: adspslpi 0xa1400000 + 0x5580000, q6_adsp_dtb 0xa1380000 + 0x80000. Firmware comes
 * from the pinned firmware pack (tools/mkfwpack.py, pandora_fw_read in sweet_ufs.c).
 *
 * Talking to the started DSP, as UniXNU-sheng's AdspProbe.cpp and AudioProbe.cpp (their SMEM,
 * SMP2P, GLINK and GPR code in bsd/dev/qcom/, unchanged): before the start, our SMP2P item 429
 * (SMEM partition apps-ADSP, allocated under TCSR mutex 3) with the entry "master-kernel";
 * after it, the DSP's item 443 entry "slave-kernel" (bit 0 fatal, 1 ready, 2 handover) within
 * 5 s; at handover the LCX/LMX votes go back to 0 (the DSP holds its own). Then GLINK on SMEM
 * items 478 (descriptor), 479 (our FIFO, 16 KiB) and 480 (the DSP's FIFO), the GPR channel
 * "adsp_apps" (DT qcom,gpr: 20 intents of 512 bytes) and the question to the DSP's audio
 * framework manager (APM) whether the framework is ready (1 s for an answer, 100 ms between
 * tries, 100 tries). Doorbells are IPCC (DT qcom,ipcc@1106000): LPASS client 3 signal 2 for
 * SMP2P and 0 for GLINK. No interrupt yet (a plain BSD file has no SPI attach): this thread
 * reads IPCC RECV_ID every 1 ms (every 20 ms after the first 30 s) and services SMP2P and
 * GLINK on every pass. pandora_adsp_gpr=2: we open "adsp_apps" (default: the DSP opens it).
 * The DSP's bus vote (bcm_vote): LP0 only, see there; pandora_adsp_wait=MS waits longer than
 * Linux's 5 s for the ready bit. Not done yet: the IPCRTR router channel. PLAN.md "扬声器路线".
 */
#if defined(PANDORA)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <arm/caches_internal.h>
#include "qcom/QcomSmem.h"
#include "qcom/QcomSmp2p.h"
#include "qcom/QcomGlink.h"
#include "qcom/ShengGpr.h"

extern kern_return_t thread_terminate(thread_t);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
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
#define AD_SMEM_PA       0x81d00000u     /* DT smem_region@81d00000, hwlocks <&tcsr_mutex 3> */
#define AD_SMEM_SIZE     0x200000u
#define AD_MUTEX_PA      0x01f43000u     /* syscon@1f40000 + lock 3 * stride 0x1000 */
#define AD_PID           2u              /* the ADSP's remote pid: SMP2P, GLINK, SMEM partition */
#define AD_ITEM_CRASH    423u
#define AD_ITEM_P2P_OUT  429u            /* DT qcom,smp2p-adsp qcom,smem = <443 429> */
#define AD_ITEM_P2P_IN   443u
#define AD_ITEM_GL_DESC  478u
#define AD_ITEM_GL_TX    479u
#define AD_ITEM_GL_RX    480u
#define AD_GL_FIFO       0x4000u

/* IPCC (drivers/mailbox/qcom-ipcc.c): id = client << 16 | signal. */
#define IPCC_SEND        0x0cu
#define IPCC_RECV_ID     0x10u
#define IPCC_RECV_ENABLE 0x14u
#define IPCC_RECV_CLEAR  0x1cu
#define IPCC_NONE        0xffffffffu
#define IPCC_AOP_QMP     ((0u << 16) | 0u)
#define IPCC_LPASS_GLINK ((3u << 16) | 0u)
#define IPCC_LPASS_SMP2P ((3u << 16) | 2u)

#define SLAVE_FATAL      1u
#define SLAVE_READY      2u
#define SLAVE_HANDOVER   4u

static int ad_chan;

static vm_offset_t
ad_map(uint64_t pa, uint64_t len, uint32_t wimg)
{
	return ml_io_map_unmappable((vm_offset_t)pa, (vm_size_t)((len + 0xfff) & ~0xfffull), wimg);
}

static uint64_t
ad_ms(uint64_t since)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time() - since, &ns);
	return ns / 1000000;
}

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
scm_smc_raw(uint64_t fn, uint64_t arginfo, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5,
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
#define SCM_WAITQ_SLEEP   2u               /* x0: the firmware queued the call (x1 queue, x2 call context) */
#define SCM_SVC_WAITQ     0x24u
#define SCM_WAITQ_RESUME  0x02u
#define SCM_WAITQ_GET_CTX 0x03u
#define SCM_WAKE_ONE      1u

/*
 * Every SCM call of this kernel (the ADSP here, the GPU through pandora_scm_call) goes through
 * scm_smc, one at a time, as Linux's qcom_scm-smc.c held qcom_scm_lock around each call. adsp1:
 * the ADSP thread's shm_bridge_enable and the GPU's is_call_avail met in TrustZone at 0.18 s; the
 * GPU's call came back WAITQ_SLEEP, nothing resumed it, and every later call came back 2.
 * A call queued all the same (another VM holding the firmware's lock) continues as
 * __scm_smc_do_quirk_handle_waitq: the wake-up is the DT qcom_scm interrupt (SPI 962), which this
 * file cannot take, so GET_WQ_CTX is asked every millisecond until it names a context to wake
 * (qcom_scm_irq_handler: flags WAKE_ONE), then WAITQ_RESUME with the call context goes on.
 */
LCK_GRP_DECLARE(ad_scm_grp, "pandora_scm");
LCK_MTX_DECLARE(ad_scm_lock, &ad_scm_grp);

static void
scm_smc(uint64_t fn, uint64_t arginfo, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5,
    struct scm_res *res)
{
	static unsigned logged;
	lck_mtx_lock(&ad_scm_lock);
	scm_smc_raw(fn, arginfo, x2, x3, x4, x5, res);
	for (unsigned n = 0; res->a0 == SCM_WAITQ_SLEEP && n < 16; n++) {
		uint64_t queue = res->a1, call = res->a2;
		struct scm_res w = {0, 0, 0, 0};
		unsigned ms = 0;
		for (; ms < 5000; ms++) {
			scm_smc_raw(SCM_FN(SCM_SVC_WAITQ, SCM_WAITQ_GET_CTX), SCM_ARGS(0), 0, 0, 0, 0, &w);
			if (w.a0 == 0 && w.a2 == SCM_WAKE_ONE) {
				break;
			}
			delay(1000);
		}
		if (logged < 16) {
			logged++;
			printf("PANDORA SCM: fn %#llx queued (queue %#llx, call %#llx); GET_WQ_CTX after %u ms -> a0 %#llx "
			    "ctx %#llx flags %#llx more %#llx\n", fn, queue, call, ms, w.a0, w.a1, w.a2, w.a3);
		}
		scm_smc_raw(SCM_FN(SCM_SVC_WAITQ, SCM_WAITQ_RESUME), SCM_ARGS(1), call, 0, 0, 0, res);
	}
	lck_mtx_unlock(&ad_scm_lock);
}

/* For bsd/dev/pandora_gpu.c: an SCM call, results in a0 and a1. */
void pandora_scm_call(uint64_t fn, uint64_t arginfo, uint64_t x2, uint64_t x3, uint64_t *a0,
    uint64_t *a1);

void
pandora_scm_call(uint64_t fn, uint64_t arginfo, uint64_t x2, uint64_t x3, uint64_t *a0,
    uint64_t *a1)
{
	struct scm_res r;
	scm_smc(fn, arginfo, x2, x3, 0, 0, &r);
	*a0 = r.a0;
	*a1 = r.a1;
}

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
static bool ad_qmp_open;

static void
qmp_kick(void)
{
	ad_wr(ad_ipcc, IPCC_SEND, IPCC_AOP_QMP);
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
	if (!ad_qmp_open) {
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
		ad_qmp_open = true;
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

/* BCM vote (include/soc/qcom/tcs.h BCM_TCS_CMD): commit, valid, x average, y peak, 14 bits each. */
#define BCM_CMD(commit, valid, x, y) \
	(((uint32_t)(commit) << 30) | ((uint32_t)(valid) << 29) | (((uint32_t)(x) & 0x3fff) << 14) | ((uint32_t)(y) & 0x3fff))
#define BCM_VOTE_MAX     0x3fffu

/*
 * The DSP's way to DDR (DT "rproc_ddr": lpass_lpicx_noc MASTER_LPASS_PROC -> mc_virt SLAVE_EBI1).
 * qcom_q6v5_prepare votes it 0/UINT_MAX before load_state and the handover takes it back. On that
 * path (Linux drivers/interconnect/qcom/kaanapali.c) only LP0 (qnm_lpass_lpinoc, qns_lpass_aggnoc)
 * serves LPASS alone; SH0 and MC0 are keepalive BCMs the apps' own DDR traffic needs, and ACV is the
 * apps' DDR mask, so those are left as the boot stages set them. UINT_MAX saturates the vote.
 */
static int
bcm_vote(const char *res, uint32_t peak)
{
	uint32_t addr = 0, aux = 0, len = 0;
	int err = cmddb_find(res, &addr, &aux, &len);
	if (err) {
		printf("PANDORA ADSP: cmd-db %s not found\n", res);
		return err;
	}
	err = rpmh_write1(addr, BCM_CMD(1, 1, 0, peak));
	printf("PANDORA ADSP: BCM %s addr %#x: peak vote %#x -> %d\n", res, addr, peak, err);
	return err;
}

/* ---- firmware pack and MDT --------------------------------------------------------- */

struct fw_ent {
	char name[48];
	uint64_t off, size;
};
static struct fw_ent ad_dir[63];
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
	if (memcmp(blk, "PANDFW01", 8) || n == 0 || n > 63) {
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
	if (strchr(base, '.')) {
		strlcpy(name, base, sizeof(name));      /* a whole image, e.g. gen80200_zap.mbn */
	} else {
		snprintf(name, sizeof(name), "%s.mdt", base);
	}
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
	/*
	 * No SHM bridge of our own: the SM8850 kernel's qcom_scm_pas_init_image says the
	 * hypervisor creates the bridge for PIL metadata itself and that a buffer already in
	 * a bridge makes the call fail (it did: -3145738 from init_image).
	 */
	struct scm_res r;
	*bridge = 0;
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
	/* The carveout (85 MiB for the ADSP) mapped for the copy only, in kernel_map space. */
	vm_offset_t mem = ad_map(mem_pa, mem_size, VM_WIMG_WCOMB);
	if (!mem) {
		printf("PANDORA ADSP: %s: carveout %#llx+%#llx not mapped\n", name, mem_pa, mem_size);
		return ENOMEM;
	}
	uint64_t reloc_base = reloc ? min : mem_pa, total = 0;
	for (unsigned i = 0; i < phnum && !err; i++) {
		if (!mdt_loadable(&ph[i])) {
			continue;
		}
		uint64_t off = (uint64_t)ph[i].p_paddr - reloc_base;
		if ((int64_t)off < 0 || off + ph[i].p_memsz > mem_size || ph[i].p_filesz > ph[i].p_memsz) {
			printf("PANDORA ADSP: %s segment %u outside the carveout\n", name, i);
			err = EINVAL;
			break;
		}
		if (ph[i].p_filesz && (err = mdt_segment(base, mdt, &ph[i], i, (void *)(mem + off)))) {
			break;
		}
		if (ph[i].p_memsz > ph[i].p_filesz) {
			bzero((void *)(mem + off + ph[i].p_filesz), ph[i].p_memsz - ph[i].p_filesz);
		}
		total += ph[i].p_filesz;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	ml_io_unmap(mem, (vm_size_t)((mem_size + 0xfff) & ~0xfffull));
	if (!err) {
		printf("PANDORA ADSP: %s: %llu bytes loaded at %#llx\n", name, total, mem_pa);
	}
	return err;
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

/* For bsd/dev/pandora_gpu.c: load FILE from the pinned pack as PAS image PAS into the
 * carveout, then authenticate and reset it. */
int pandora_pas_load_auth(const char *file, unsigned pas, uint64_t mem_pa, uint64_t mem_size);

int
pandora_pas_load_auth(const char *file, unsigned pas, uint64_t mem_pa, uint64_t mem_size)
{
	uint64_t bridge = 0;
	int err = ad_nfiles ? 0 : fw_dir();
	if (!err) {
		err = pas_load(file, pas, mem_pa, mem_size, &bridge);
	}
	if (!err) {
		err = pas_auth(pas);
	}
	return err;
}

/* ---- SMEM items and IPCC doorbells -------------------------------------------------- */

static vm_offset_t ad_smem_va, ad_mutex;
static QcomSmem ad_smem;
static QcomSmemPartition ad_part;
static uint32_t ad_ipcc_n[4];            /* doorbells seen: SMP2P, GLINK, AOP, other */
static uint32_t ad_ipcc_other;

/* TCSR mutex (drivers/hwspinlock/qcom_hwspinlock.c): write our owner id 1, read it back. */
static bool
smem_lock(void)
{
	for (int i = 0; i < 1000; i++) {
		ad_wr(ad_mutex, 0, 1);
		if (ad_rd(ad_mutex, 0) == 1) {
			return true;
		}
		delay(10);
	}
	return false;
}

static int
smem_alloc(uint16_t item, uint32_t size)
{
	if (!smem_lock()) {
		return EBUSY;
	}
	int rc = qcom_smem_alloc(&ad_smem, &ad_part, item, size);
	ad_wr(ad_mutex, 0, 0);
	if (rc && rc != QcomSmemExists) {
		printf("PANDORA ADSP: SMEM item %u (%u bytes) not allocated: %d\n", item, size, rc);
		return EIO;
	}
	return 0;
}

static volatile uint8_t *
smem_item(uint16_t item, uint32_t *size)
{
	uint32_t at = 0, bytes = 0;
	if (qcom_smem_find(&ad_smem, &ad_part, item, &at, &bytes)) {
		return NULL;
	}
	if (size) {
		*size = bytes;
	}
	return (volatile uint8_t *)ad_smem_va + at;
}

/* Which of our items the partition apps-ADSP holds, item:size. */
static void
smem_survey(const char *when)
{
	static const uint16_t items[5] = {AD_ITEM_P2P_IN, AD_ITEM_P2P_OUT, AD_ITEM_GL_DESC, AD_ITEM_GL_TX, AD_ITEM_GL_RX};
	char have[64];
	have[0] = 0;
	for (unsigned i = 0; i < 5; i++) {
		uint32_t size = 0;
		if (smem_item(items[i], &size)) {
			char one[16];
			snprintf(one, sizeof(one), " %u:%u", items[i], size);
			strlcat(have, one, sizeof(have));
		}
	}
	printf("PANDORA ADSP: SMEM %s: partition at %#x size %#x hosts %u/%u, cacheline %u; items there:%s\n", when,
	    ad_part.offset, ad_part.size, ad_part.host0, ad_part.host1, ad_part.cacheline, have[0] ? have : " none");
}

/* The DSP's crash text (global partition item 423, qcom_q6v5.c q6v5_fatal_interrupt). */
static void
smem_crash_reason(void)
{
	QcomSmemPartition global;
	uint32_t at = 0, size = 0;
	if (qcom_smem_partition(&ad_smem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &global) ||
	    qcom_smem_find(&ad_smem, &global, AD_ITEM_CRASH, &at, &size) || !size) {
		printf("PANDORA ADSP: no crash reason in SMEM\n");
		return;
	}
	char text[160];
	unsigned n = 0;
	for (; n < sizeof(text) - 1 && n < size; n++) {
		char c = (char)((volatile uint8_t *)ad_smem_va)[at + n];
		if (!c) {
			break;
		}
		text[n] = (c >= 32 && c < 127) ? c : '.';
	}
	text[n] = 0;
	printf("PANDORA ADSP: crash reason: \"%s\"\n", text);
}

/* Takes what rang: bit 0 SMP2P, bit 1 GLINK. */
static unsigned
ipcc_poll(void)
{
	unsigned bits = 0;
	for (int n = 0; n < 32; n++) {
		uint32_t id = ad_rd(ad_ipcc, IPCC_RECV_ID);
		if (id == IPCC_NONE) {
			break;
		}
		ad_wr(ad_ipcc, IPCC_RECV_CLEAR, id);
		if (id == IPCC_LPASS_SMP2P) {
			ad_ipcc_n[0]++;
			bits |= 1;
		} else if (id == IPCC_LPASS_GLINK) {
			ad_ipcc_n[1]++;
			bits |= 2;
		} else if (id == IPCC_AOP_QMP) {
			ad_ipcc_n[2]++;
		} else {
			ad_ipcc_n[3]++;
			ad_ipcc_other = id;
		}
	}
	return bits;
}

/* ---- SMP2P ---------------------------------------------------------------------------- */

static QcomSmp2p ad_p2p;
static uint32_t ad_slave;
static bool ad_handed;
static uint64_t ad_t0;                   /* main image auth_and_reset */

static void
p2p_kick(void)
{
	ad_wr(ad_ipcc, IPCC_SEND, IPCC_LPASS_SMP2P);
}

static void
p2p_service(void)
{
	if (!ad_p2p.in) {
		uint32_t size = 0;
		volatile uint8_t *in = smem_item(AD_ITEM_P2P_IN, &size);
		if (!in) {
			return;
		}
		ad_p2p.in = (volatile uint32_t *)in;
		printf("PANDORA ADSP: SMP2P item %u (%u bytes) there after %llu ms: check %d\n", AD_ITEM_P2P_IN, size,
		    ad_t0 ? ad_ms(ad_t0) : 0, qcom_smp2p_check_in(ad_p2p.in, 0, AD_PID));
	}
	uint32_t value = 0, changed = 0;
	if (qcom_smp2p_service(&ad_p2p, "slave-kernel", &value, &changed)) {
		p2p_kick();
	}
	if (changed) {
		printf("PANDORA ADSP: slave-kernel %#x (changed %#x) after %llu ms\n", value, changed,
		    ad_t0 ? ad_ms(ad_t0) : 0);
	}
	ad_slave = value;
	if (changed & value & SLAVE_FATAL) {
		smem_crash_reason();
	}
	if ((value & SLAVE_HANDOVER) && !ad_handed) {
		/* qcom_pas_handover and q6v5_handover_interrupt: the proxy votes go, the DSP holds its own now. */
		ad_handed = true;
		(void)arc_vote("lcx.lvl", 0);
		(void)arc_vote("lmx.lvl", 0);
		(void)bcm_vote("LP0", 0);
	}
}

/* ---- GLINK and the GPR channel ------------------------------------------------------- */

#define GPR_INTENT       512u            /* DT qcom,gpr qcom,intents = <0x200 0x14> */
#define GPR_INTENTS      20u
#define GPR_ANSWER_MS    1000u
#define GPR_RETRY_MS     100u
#define GPR_QUERIES      100u

static QcomGlink ad_gl;
static QcomGlinkChannel *ad_gpr;
static uint8_t ad_gpr_buf[GPR_INTENT * GPR_INTENTS];
static GprSpfProbe ad_spf;
static bool ad_gpr_up, ad_gpr_changed, ad_spf_done;
static uint64_t ad_gpr_up_at, ad_answer_by, ad_retry_at;
static uint8_t ad_tx[GPR_HEADER_BYTES];
static uint32_t ad_tx_len;
/* Packets from the channel, handled after the poll (the callbacks run inside it). */
static struct {
	uint32_t len;
	uint8_t data[64];
} ad_inbox[4];
static unsigned ad_inbox_n, ad_inbox_lost;

static void
gl_kick(void *ctx)
{
	(void)ctx;
	ad_wr(ad_ipcc, IPCC_SEND, IPCC_LPASS_GLINK);
}

static void
gpr_state(void *ctx, QcomGlinkChannel *ch, int up)
{
	(void)ctx;
	(void)ch;
	ad_gpr_up = up != 0;
	ad_gpr_changed = true;
}

static void
gpr_receive(void *ctx, QcomGlinkChannel *ch, const uint8_t *data, uint32_t len)
{
	(void)ctx;
	(void)ch;
	if (ad_inbox_n == 4) {
		ad_inbox_lost++;
		return;
	}
	ad_inbox[ad_inbox_n].len = MIN(len, (uint32_t)sizeof(ad_inbox[0].data));
	memcpy(ad_inbox[ad_inbox_n].data, data, ad_inbox[ad_inbox_n].len);
	ad_inbox_n++;
}

static int
gl_start(int open_gpr)
{
	int err = smem_alloc(AD_ITEM_GL_DESC, 32);
	if (!err) {
		err = smem_alloc(AD_ITEM_GL_TX, AD_GL_FIFO);
	}
	uint32_t dsize = 0, tsize = 0;
	volatile uint8_t *desc = smem_item(AD_ITEM_GL_DESC, &dsize), *txf = smem_item(AD_ITEM_GL_TX, &tsize);
	printf("PANDORA ADSP: GLINK items %u (%u bytes) %u (%u bytes) -> %d\n", AD_ITEM_GL_DESC, dsize, AD_ITEM_GL_TX,
	    tsize, err);
	if (err || !desc || !txf || dsize < 32) {
		return err ? err : ENOENT;
	}
	/* qcom_glink_smem.c: descriptor words 0/1 our FIFO's tail/head, 2/3 the DSP's. */
	volatile uint32_t *d = (volatile uint32_t *)desc;
	QcomGlinkFifo tx = {&d[1], &d[0], txf, tsize}, rx = {&d[3], &d[2], NULL, 0};
	qcom_glink_init(&ad_gl, tx, rx, gl_kick, NULL);
	static const QcomGlinkClient client = {gpr_state, gpr_receive};
	ad_gpr = qcom_glink_channel(&ad_gl, "adsp_apps", GPR_INTENT, GPR_INTENTS, ad_gpr_buf, &client, NULL, open_gpr);
	return ad_gpr ? 0 : ENOMEM;
}

static void
gpr_ask(void)
{
	uint32_t n = gpr_spf_next(&ad_spf, ad_tx);
	if (n) {
		ad_tx_len = n;
		clock_interval_to_deadline(GPR_ANSWER_MS, NSEC_PER_MSEC, &ad_answer_by);
	}
}

static void
gpr_outcome(int result)
{
	if (result == SpfIsReady) {
		printf("PANDORA ADSP: APM ready %llu ms after the channel came up (%u queries, %u answers)\n",
		    ad_ms(ad_gpr_up_at), ad_spf.queries, ad_spf.answers);
		ad_spf_done = true;
		ad_answer_by = 0;
	} else if (result == SpfDone) {
		printf("PANDORA ADSP: APM never ready: %u queries, %u answers, %u refusals (last status %#x), %u timeouts\n",
		    ad_spf.queries, ad_spf.answers, ad_spf.errors, ad_spf.last_status, ad_spf.timeouts);
		ad_spf_done = true;
		ad_answer_by = 0;
	} else if (result == SpfNotReady || result == SpfError) {
		if (ad_spf.answers + ad_spf.errors <= 3) {
			printf("PANDORA ADSP: APM answer %s status %#x\n", result == SpfError ? "refused" : "not ready",
			    ad_spf.last_status);
		}
		ad_answer_by = 0;
		clock_interval_to_deadline(GPR_RETRY_MS, NSEC_PER_MSEC, &ad_retry_at);
	}
}

/* Channel state, the packets that came in, the timers, the query waiting to go out. */
static void
gpr_step(void)
{
	uint64_t now = mach_absolute_time();
	if (ad_gpr_changed) {
		ad_gpr_changed = false;
		printf("PANDORA ADSP: channel adsp_apps %s after %llu ms\n", ad_gpr_up ? "up" : "down", ad_ms(ad_t0));
		ad_answer_by = ad_retry_at = 0;
		ad_tx_len = 0;
		if (ad_gpr_up && !ad_spf_done) {
			ad_gpr_up_at = now;
			gpr_spf_init(&ad_spf, GPR_PORT_APM, GPR_QUERIES);
			gpr_ask();
		}
	}
	for (unsigned i = 0; i < ad_inbox_n; i++) {
		GprPacket p;
		if (!gpr_decode(ad_inbox[i].data, ad_inbox[i].len, &p) && ad_spf.answers + ad_spf.ignored < 8) {
			printf("PANDORA ADSP: GPR in: opcode %#x token %u from domain %u port %#x, %u bytes\n", p.opcode, p.token,
			    p.src_domain, p.src_port, p.size);
		}
		if (ad_gpr_up && !ad_spf_done) {
			gpr_outcome(gpr_spf_receive(&ad_spf, ad_inbox[i].data, ad_inbox[i].len));
		}
	}
	ad_inbox_n = 0;
	if (!ad_gpr_up || ad_spf_done) {
		return;
	}
	if (ad_answer_by && now >= ad_answer_by) {
		ad_answer_by = 0;
		ad_tx_len = 0;
		if (gpr_spf_timeout(&ad_spf) == SpfDone) {
			gpr_outcome(SpfDone);
			return;
		}
		gpr_ask();
	}
	if (ad_retry_at && now >= ad_retry_at) {
		ad_retry_at = 0;
		gpr_ask();
	}
	if (ad_tx_len && qcom_glink_send(&ad_gl, ad_gpr, ad_tx, ad_tx_len) == QcomGlinkOk) {
		ad_tx_len = 0;
	}
}

static void
gl_service(void)
{
	if (!ad_gl.rx.data) {
		uint32_t size = 0;
		volatile uint8_t *rx = smem_item(AD_ITEM_GL_RX, &size);
		if (rx) {
			qcom_glink_set_rx(&ad_gl, rx, size);
			printf("PANDORA ADSP: GLINK item %u (%u bytes) there after %llu ms\n", AD_ITEM_GL_RX, size, ad_ms(ad_t0));
		}
	}
	(void)qcom_glink_poll(&ad_gl);
	gpr_step();
}

/* What changed in the link since the last report. */
static void
gl_report(bool force)
{
	static uint8_t agreed, broken;
	static uint32_t unknown, ipcc_sum;
	uint32_t sum = ad_ipcc_n[0] + ad_ipcc_n[1] + ad_ipcc_n[2] + ad_ipcc_n[3];
	if (!force && agreed == ad_gl.version_agreed && broken == ad_gl.broken && unknown == ad_gl.unknown_opens &&
	    ipcc_sum == sum) {
		return;
	}
	agreed = ad_gl.version_agreed;
	broken = ad_gl.broken;
	unknown = ad_gl.unknown_opens;
	ipcc_sum = sum;
	ad_gl.unknown_names[sizeof(ad_gl.unknown_names) - 1] = 0;
	printf("PANDORA ADSP: at %llu ms GLINK version %s%s, messages in %u out %u data %u dropped %u; opens nobody "
	    "serves %u (%s); doorbells SMP2P %u GLINK %u AOP %u other %u (last %#x); inbox lost %u\n", ad_ms(ad_t0),
	    agreed ? "agreed" : "not agreed", broken ? ", LINK BROKEN" : "", ad_gl.rx_messages, ad_gl.tx_messages,
	    ad_gl.rx_data, ad_gl.dropped, unknown, ad_gl.unknown_names, ad_ipcc_n[0], ad_ipcc_n[1], ad_ipcc_n[2],
	    ad_ipcc_n[3], ad_ipcc_other, ad_inbox_lost);
}

/* Windows, the SMEM partition, what the boot stages left, our SMP2P item, the doorbells. */
static int
adsp_comms_setup(void)
{
	ad_qmp = ad_map(AD_QMP_PA, 0x400, VM_WIMG_IO);
	ad_ipcc = ad_map(AD_IPCC_PA, 0x1000, VM_WIMG_IO);
	ad_rsc = ad_map(AD_RSC_PA, 0x10000, VM_WIMG_IO);
	ad_db = ad_map(AD_CMDDB_PA, AD_CMDDB_SIZE, VM_WIMG_IO);
	ad_smem_va = ad_map(AD_SMEM_PA, AD_SMEM_SIZE, VM_WIMG_IO);
	ad_mutex = ad_map(AD_MUTEX_PA, 0x1000, VM_WIMG_IO);
	if (!ad_qmp || !ad_ipcc || !ad_rsc || !ad_db || !ad_smem_va || !ad_mutex) {
		printf("PANDORA ADSP: windows not mapped\n");
		return ENOMEM;
	}
	ad_smem.base = (volatile uint8_t *)ad_smem_va;
	ad_smem.size = AD_SMEM_SIZE;
	int rc = qcom_smem_partition(&ad_smem, QCOM_SMEM_HOST_APPS, AD_PID, &ad_part);
	if (rc) {
		printf("PANDORA ADSP: no SMEM partition apps-ADSP: %d\n", rc);
		return ENODEV;
	}
	smem_survey("before the start");
	volatile uint8_t *in = smem_item(AD_ITEM_P2P_IN, NULL);
	if (in) {
		QcomSmp2p view;
		bzero(&view, sizeof(view));
		view.in = (volatile uint32_t *)in;
		int index = qcom_smp2p_check_in(view.in, 0, AD_PID) ? -1 : qcom_smp2p_find_in(&view, "slave-kernel");
		printf("PANDORA ADSP: SMP2P from an earlier start: slave-kernel %#x\n",
		    index >= 0 ? qcom_smp2p_in_value(&view, index) : 0xffffffffu);
	}
	/* Our item before the DSP starts: it looks for it when it comes up. */
	if ((rc = smem_alloc(AD_ITEM_P2P_OUT, QCOM_SMP2P_ITEM_BYTES))) {
		return rc;
	}
	uint32_t osize = 0;
	volatile uint8_t *out = smem_item(AD_ITEM_P2P_OUT, &osize);
	if (!out || osize < QCOM_SMP2P_ITEM_BYTES) {
		printf("PANDORA ADSP: SMP2P item %u is %u bytes\n", AD_ITEM_P2P_OUT, osize);
		return EIO;
	}
	qcom_smp2p_init_out(&ad_p2p, (volatile uint32_t *)out, 0, AD_PID);
	int stop = qcom_smp2p_add_entry(&ad_p2p, "master-kernel");
	static const uint32_t ids[3] = {IPCC_LPASS_SMP2P, IPCC_LPASS_GLINK, IPCC_AOP_QMP};
	for (unsigned i = 0; i < 3; i++) {
		ad_wr(ad_ipcc, IPCC_RECV_ENABLE, ids[i]);
	}
	(void)ipcc_poll();
	bzero(ad_ipcc_n, sizeof(ad_ipcc_n));
	p2p_kick();
	printf("PANDORA ADSP: SMP2P item %u made (entry master-kernel %d), doorbells enabled\n", AD_ITEM_P2P_OUT, stop);
	return 0;
}

/* After the main image's reset: ready (or fatal) within 5 s, then GLINK and the APM question. */
static void
adsp_talk(int open_gpr)
{
	/* qcom_q6v5_wait_for_start: 5 s; pandora_adsp_wait=MS waits longer, to tell slow from dead. */
	uint64_t until;
	unsigned wait_ms = 5000;
	(void)PE_parse_boot_argn("pandora_adsp_wait", &wait_ms, sizeof(wait_ms));
	clock_interval_to_deadline(wait_ms, NSEC_PER_MSEC, &until);
	while (!(ad_slave & (SLAVE_READY | SLAVE_FATAL)) && mach_absolute_time() < until) {
		(void)ipcc_poll();
		p2p_service();
		ad_sleep(1);
	}
	gl_report(true);
	smem_survey("after the wait");
	if (!(ad_slave & SLAVE_READY) || (ad_slave & SLAVE_FATAL)) {
		printf("PANDORA ADSP: not ready after %llu ms (slave-kernel %#x)\n", ad_ms(ad_t0), ad_slave);
		smem_crash_reason();
		return;
	}
	printf("PANDORA ADSP: ready after %llu ms\n", ad_ms(ad_t0));
	if (gl_start(open_gpr)) {
		return;
	}
	/* From here on this thread only answers: every 1 ms for 30 s, then every 20 ms. */
	uint64_t fast_until, next_report;
	clock_interval_to_deadline(30000, NSEC_PER_MSEC, &fast_until);
	clock_interval_to_deadline(5000, NSEC_PER_MSEC, &next_report);
	for (;;) {
		(void)ipcc_poll();
		p2p_service();
		gl_service();
		uint64_t now = mach_absolute_time();
		gl_report(now >= next_report);
		if (now >= next_report) {
			clock_interval_to_deadline(now < fast_until ? 5000 : 60000, NSEC_PER_MSEC, &next_report);
		}
		ad_sleep(now < fast_until ? 1 : 20);
	}
}

static void
adsp_boot(void)
{
	int err;
	uint64_t b_dtb = 0, b_main = 0;
	struct scm_res r;
	int open_gpr = 0;
	(void)PE_parse_boot_argn("pandora_adsp_gpr", &open_gpr, sizeof(open_gpr));
	if ((err = fw_dir()) || (err = adsp_comms_setup())) {
		return;
	}
	/* qcom_pas_start's order: qcom_q6v5_prepare (bus vote, load_state), then the proxy power
	 * domains (DT cx/mx-supply, cx/mx-uV-uA 384). */
	(void)bcm_vote("LP0", BCM_VOTE_MAX);
	if ((err = qmp_send("{class: image, res: load_state, name: adsp, val: on}"))) {
		printf("PANDORA ADSP: load_state failed %d\n", err);
	}
	(void)arc_vote("lcx.lvl", 384);
	(void)arc_vote("lmx.lvl", 384);
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
	if ((err = pas_load("adsp", AD_PAS_MAIN, AD_MEM_PA, AD_MEM_SIZE, &b_main))) {
		printf("PANDORA ADSP: main image failed %d\n", err);
		return;
	}
	ad_t0 = mach_absolute_time();
	if ((err = pas_auth(AD_PAS_MAIN))) {
		printf("PANDORA ADSP: main image failed %d\n", err);
		return;
	}
	printf("PANDORA ADSP: ADSP released from reset\n");
	adsp_talk(open_gpr == 2);
	/* adsp_talk returns only when the DSP did not come up: stop it, as qcom_pas_start's error path. */
	scm_smc(SCM_FN(2, 6), SCM_ARGS(1), AD_PAS_MAIN, 0, 0, 0, &r);
	printf("PANDORA ADSP: pas %u shutdown -> a0 %#llx a1 %#llx\n", AD_PAS_MAIN, r.a0, r.a1);
	scm_smc(SCM_FN(2, 6), SCM_ARGS(1), AD_PAS_DTB, 0, 0, 0, &r);
	printf("PANDORA ADSP: pas %u shutdown -> a0 %#llx a1 %#llx\n", AD_PAS_DTB, r.a0, r.a1);
	if (!ad_handed) {
		(void)arc_vote("lcx.lvl", 0);
		(void)arc_vote("lmx.lvl", 0);
		(void)bcm_vote("LP0", 0);
	}
	(void)qmp_send("{class: image, res: load_state, name: adsp, val: off}");   /* qcom_q6v5_unprepare */
	(void)b_main;
	(void)b_dtb;
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

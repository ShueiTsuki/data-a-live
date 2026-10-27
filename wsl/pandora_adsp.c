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
 * from the device's firmware partition, else the pinned pack ("firmware files" below).
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
 * Linux's 5 s for the ready bit. The battery: the SOCCP's battery manager over PMIC GLINK ("the
 * battery" below), boot-arg pandora_battery=1. Not done yet: the IPCRTR router channel. PLAN.md
 * "扬声器路线".
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
#include <pexpert/device_tree.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <arm/caches_internal.h>
#include "qcom/QcomSmem.h"
#include "qcom/QcomSmp2p.h"
#include "qcom/QcomGlink.h"
#include "qcom/ShengGpr.h"
#include "qcom/FatVolume.h"
#include "pandora_battery.h"

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

/* ---- firmware files and MDT ---------------------------------------------------------- */

/*
 * The ADSP images come from the device's own firmware partition, as Android and UniXNU-sheng
 * take them: /image of modem_<slot> (Android's /vendor/firmware_mnt), a FAT volume read in place
 * through sweet_ufs.c (pandora_part_find, pandora_part_pread; read only) with UniXNU-sheng's
 * FatVolume.c. The slot is boot-arg pandora_slot=a|b (tools/boot_ios.sh passes Android's
 * ro.boot.slot_suffix): after an update the other slot's partition may hold another build.
 * pandora_adsp_fw=pack, or no usable partition: the pinned pack (tools/mkfwpack.py,
 * pandora_fw_read in sweet_ufs.c), which also carries the GPU's files (their vendor partition is
 * not FAT).
 */
extern int pandora_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks);
extern int pandora_part_pread(uint64_t off, void *dst, uint64_t len);

struct fw_ent {
	char name[48];
	uint64_t off, size;
};
static struct fw_ent ad_dir[63];
static unsigned ad_nfiles;
static FatVolume ad_fat;
static int ad_fat_state;                 /* 0 not tried, 1 mounted, -1 not usable */

/* One file: in the firmware partition's FAT, or in the pack. */
struct fw_file {
	uint64_t size, pack_off;
	FatFile fat;
	bool in_fat;
};

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

static int
fat_part_read(void *ctx, uint64_t off, void *to, size_t len)
{
	(void)ctx;
	return pandora_part_pread(off, to, len);
}

/* The firmware partition's FAT, once. */
static int
fw_mount(void)
{
	if (ad_fat_state) {
		return ad_fat_state > 0 ? 0 : ENOENT;
	}
	ad_fat_state = -1;
	char slot[4] = "", part[16];
	if (!PE_parse_boot_arg_str("pandora_slot", slot, sizeof(slot)) || (slot[0] != 'a' && slot[0] != 'b') ||
	    slot[1]) {
		printf("PANDORA ADSP: no boot-arg pandora_slot=a|b: the firmware partition is not read\n");
		return ENOENT;
	}
	snprintf(part, sizeof(part), "modem_%c", slot[0]);
	uint32_t lun = 0;
	uint64_t first = 0, blocks = 0;
	int err = pandora_part_find(part, &lun, &first, &blocks);
	if (err) {
		printf("PANDORA ADSP: partition %s not found: %d\n", part, err);
		return err;
	}
	int rc = fat_mount(&ad_fat, blocks * 4096, fat_part_read, NULL);
	printf("PANDORA ADSP: %s: LUN %u blocks %#llx+%#llx, FAT mount -> %d (FAT%s, %u-byte clusters)\n", part, lun,
	    first, blocks, rc, ad_fat.fat32 ? "32" : "16", ad_fat.cluster_bytes);
	if (rc) {
		return EFTYPE;
	}
	ad_fat_state = 1;
	return 0;
}

static int
fw_open(const char *name, bool part, struct fw_file *f)
{
	bzero(f, sizeof(*f));
	if (part) {
		char path[64];
		snprintf(path, sizeof(path), "image/%s", name);
		int rc = fat_open(&ad_fat, path, &f->fat);
		if (rc || f->fat.directory) {
			return rc == FatNotFound ? ENOENT : EIO;
		}
		f->size = f->fat.size;
		f->in_fat = true;
		return 0;
	}
	struct fw_ent *e = fw_find(name);
	if (!e) {
		return ENOENT;
	}
	f->size = e->size;
	f->pack_off = e->off;
	return 0;
}

static int
fw_pread(const struct fw_file *f, uint64_t off, void *dst, uint64_t len)
{
	if (off + len < off || off + len > f->size) {
		return EINVAL;
	}
	if (f->in_fat) {
		return fat_read(&ad_fat, &f->fat, off, dst, (size_t)len) ? EIO : 0;
	}
	return pandora_fw_read(f->pack_off + off, dst, len);
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
mdt_segment(const char *base, bool part, const struct fw_file *mdt, const struct elf32_phdr *p, unsigned i,
    void *dst)
{
	char name[48];
	struct fw_file seg;
	snprintf(name, sizeof(name), "%s.b%02u", base, i);
	if (!fw_open(name, part, &seg)) {
		if (seg.size != p->p_filesz) {
			printf("PANDORA ADSP: %s is %llu bytes, segment wants %u\n", name, seg.size, p->p_filesz);
			return EINVAL;
		}
		return fw_pread(&seg, 0, dst, p->p_filesz);
	}
	if ((uint64_t)p->p_offset + p->p_filesz <= mdt->size) {
		return fw_pread(mdt, p->p_offset, dst, p->p_filesz);
	}
	printf("PANDORA ADSP: segment %u of %s missing\n", i, base);
	return ENOENT;
}

/*
 * One image: metadata to TrustZone through an SHM bridge, init, mem setup, segments into
 * the carveout. The bridge handle is returned for deletion after the reset.
 */
static int
pas_load(const char *base, bool part, unsigned pas, uint64_t mem_pa, uint64_t mem_size, uint64_t *bridge)
{
	char name[48];
	if (strchr(base, '.')) {
		strlcpy(name, base, sizeof(name));      /* a whole image, e.g. gen80200_zap.mbn */
	} else {
		snprintf(name, sizeof(name), "%s.mdt", base);
	}
	struct fw_file mdt;
	if (fw_open(name, part, &mdt) || mdt.size < 52 || mdt.size > 0x10000) {
		printf("PANDORA ADSP: %s missing in the %s\n", name, part ? "firmware partition" : "pack");
		return ENOENT;
	}
	static uint8_t hdr[0x10000];
	int err = fw_pread(&mdt, 0, hdr, mdt.size);
	if (err) {
		return err;
	}
	uint32_t phoff, phnum;
	memcpy(&phoff, hdr + 28, 4);
	phnum = hdr[44] | (hdr[45] << 8);
	if (memcmp(hdr, "\177ELF", 4) || hdr[4] != 1 || phnum < 2 || phnum > 128 ||
	    phoff + phnum * sizeof(struct elf32_phdr) > mdt.size) {
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
	printf("PANDORA ADSP: %s (%s): %u segments, hash %u (%u bytes), load %#llx-%#llx%s\n", name,
	    part ? "firmware partition" : "pack", phnum, hash, hash_size, min, max, reloc ? " relocatable" : "");
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
	if (ehdr_size + hash_size == mdt.size) {
		memcpy((void *)(md + ehdr_size), hdr + ehdr_size, hash_size);
	} else if ((uint64_t)ph[hash].p_offset + hash_size <= mdt.size) {
		memcpy((void *)(md + ehdr_size), hdr + ph[hash].p_offset, hash_size);
	} else if ((err = mdt_segment(base, part, &mdt, &ph[hash], hash, (void *)(md + ehdr_size)))) {
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
		if (ph[i].p_filesz && (err = mdt_segment(base, part, &mdt, &ph[i], i, (void *)(mem + off)))) {
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
		err = pas_load(file, false, pas, mem_pa, mem_size, &bridge);
	}
	if (!err) {
		err = pas_auth(pas);
	}
	return err;
}

/* ---- the remote processors' links: SMEM, IPCC doorbells, SMP2P, GLINK ----------------- */

/*
 * One link per remote processor, as Linux has one qcom,smp2p node, one glink-edge and one SMEM
 * partition for each: the ADSP (pid 2, SMP2P 443 in / 429 out, IPCC client 3), which this file
 * starts, and the SOCCP (pid 19, SMP2P 617 / 616, IPCC client 13; mainline kaanapali.dtsi
 * smp2p-soccp and remoteproc_soccp), which the boot loaders start and Android only attaches to
 * (its remoteproc state is "attached"). One thread owns both links and reads IPCC for both.
 */
#define SO_PID           19u
#define SO_ITEM_P2P_IN   617u
#define SO_ITEM_P2P_OUT  616u
#define SO_ITEM_CRASH    656u            /* kaanapali_soccp_resource crash_reason_smem */
#define IPCC_SIG_GLINK   0u
#define IPCC_SIG_SMP2P   2u

struct edge {
	const char *name;
	uint16_t pid, item_in, item_out, item_crash;
	uint32_t client;                         /* IPCC client: id = client << 16 | signal */
	QcomSmemPartition part;
	QcomSmp2p p2p;
	bool p2p_on, gl_on;
	int out_entry;                           /* our "master-kernel" entry */
	uint32_t slave;
	uint64_t t0;                             /* what this link's log times count from */
	QcomGlink gl;
	uint32_t bells[2];                       /* doorbells seen: SMP2P, GLINK */
	struct {
		uint8_t agreed, broken;
		uint32_t unknown, bells;
	} shown;                                 /* what gl_report printed last */
};
static struct edge ed_adsp = {.name = "ADSP", .pid = AD_PID, .item_in = AD_ITEM_P2P_IN,
	                      .item_out = AD_ITEM_P2P_OUT, .item_crash = AD_ITEM_CRASH, .client = 3};
static struct edge ed_soccp = {.name = "SOCCP", .pid = SO_PID, .item_in = SO_ITEM_P2P_IN,
	                       .item_out = SO_ITEM_P2P_OUT, .item_crash = SO_ITEM_CRASH, .client = 13};
static struct edge *const ad_edges[2] = {&ed_adsp, &ed_soccp};

static vm_offset_t ad_smem_va, ad_mutex;
static QcomSmem ad_smem;
static uint32_t ad_ipcc_aop, ad_ipcc_other, ad_ipcc_last;
static bool ad_mapped;

/* Windows both links use, once. */
static int
rproc_map(void)
{
	if (ad_mapped) {
		return 0;
	}
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
	ad_wr(ad_ipcc, IPCC_RECV_ENABLE, IPCC_AOP_QMP);
	ad_mapped = true;
	return 0;
}

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
smem_alloc(struct edge *e, uint16_t item, uint32_t size)
{
	if (!smem_lock()) {
		return EBUSY;
	}
	int rc = qcom_smem_alloc(&ad_smem, &e->part, item, size);
	ad_wr(ad_mutex, 0, 0);
	if (rc && rc != QcomSmemExists) {
		printf("PANDORA %s: SMEM item %u (%u bytes) not allocated: %d\n", e->name, item, size, rc);
		return EIO;
	}
	return 0;
}

static volatile uint8_t *
smem_item(struct edge *e, uint16_t item, uint32_t *size)
{
	uint32_t at = 0, bytes = 0;
	if (qcom_smem_find(&ad_smem, &e->part, item, &at, &bytes)) {
		return NULL;
	}
	if (size) {
		*size = bytes;
	}
	return (volatile uint8_t *)ad_smem_va + at;
}

/* Which of the link's items its partition holds, item:size. */
static void
smem_survey(struct edge *e, const char *when)
{
	const uint16_t items[5] = {e->item_in, e->item_out, AD_ITEM_GL_DESC, AD_ITEM_GL_TX, AD_ITEM_GL_RX};
	char have[64];
	have[0] = 0;
	for (unsigned i = 0; i < 5; i++) {
		uint32_t size = 0;
		if (smem_item(e, items[i], &size)) {
			char one[16];
			snprintf(one, sizeof(one), " %u:%u", items[i], size);
			strlcat(have, one, sizeof(have));
		}
	}
	printf("PANDORA %s: SMEM %s: partition at %#x size %#x hosts %u/%u, cacheline %u; items there:%s\n", e->name,
	    when, e->part.offset, e->part.size, e->part.host0, e->part.host1, e->part.cacheline, have[0] ? have : " none");
}

/* The processor's crash text (global partition; qcom_q6v5.c q6v5_fatal_interrupt). */
static void
smem_crash_reason(struct edge *e)
{
	QcomSmemPartition global;
	uint32_t at = 0, size = 0;
	if (qcom_smem_partition(&ad_smem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &global) ||
	    qcom_smem_find(&ad_smem, &global, e->item_crash, &at, &size) || !size) {
		printf("PANDORA %s: no crash reason in SMEM\n", e->name);
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
	printf("PANDORA %s: crash reason: \"%s\"\n", e->name, text);
}

/*
 * Takes and counts what rang; returns how many. The links are serviced on every pass of the thread
 * whatever rang. Called by the thread and, with the interrupt on, at interrupt time
 * (pandora_ipcc_irq): the counters are atomic.
 */
static unsigned
ipcc_poll(void)
{
	unsigned taken = 0;
	for (int n = 0; n < 32; n++) {
		uint32_t id = ad_rd(ad_ipcc, IPCC_RECV_ID);
		if (id == IPCC_NONE) {
			break;
		}
		ad_wr(ad_ipcc, IPCC_RECV_CLEAR, id);
		taken++;
		bool known = false;
		for (unsigned i = 0; i < 2 && !known; i++) {
			struct edge *e = ad_edges[i];
			if (id == ((e->client << 16) | IPCC_SIG_SMP2P)) {
				__atomic_fetch_add(&e->bells[0], 1, __ATOMIC_RELAXED);
				known = true;
			} else if (id == ((e->client << 16) | IPCC_SIG_GLINK)) {
				__atomic_fetch_add(&e->bells[1], 1, __ATOMIC_RELAXED);
				known = true;
			}
		}
		if (known) {
			continue;
		}
		if (id == IPCC_AOP_QMP) {
			__atomic_fetch_add(&ad_ipcc_aop, 1, __ATOMIC_RELAXED);
		} else {
			__atomic_fetch_add(&ad_ipcc_other, 1, __ATOMIC_RELAXED);
			ad_ipcc_last = id;
		}
	}
	return taken;
}

/*
 * 0232: the IPCC interrupt (iokit/Kernel/arm/PandoraIpcc.cpp, INTID 278 level). Its filter calls
 * pandora_ipcc_irq, which takes what rang (clearing it lowers the line) and wakes the thread
 * (ad_wait). 64 interrupts in a row with nothing pending turn it off for good, as the GPU's does,
 * and the thread polls again. Until the source is there and the window is mapped, the thread polls.
 */
static volatile uint32_t ad_irq_pending;
static void (*ad_irq_enable)(void);
static int ad_irq_state;                 /* 0 off, 1 starting, 2 on, 3 given up */
static uint32_t ad_irq_count, ad_irq_empty, ad_irq_empty_run;

int pandora_ipcc_irq(void);
int
pandora_ipcc_irq(void)
{
	ad_irq_count++;
	if (!ad_ipcc || !ipcc_poll()) {
		ad_irq_empty++;
		if (++ad_irq_empty_run >= 64) {
			__atomic_store_n(&ad_irq_state, 3, __ATOMIC_RELEASE);
			thread_wakeup((event_t)&ad_irq_pending);
			return 1;
		}
		return 0;
	}
	ad_irq_empty_run = 0;
	__atomic_store_n(&ad_irq_pending, 1, __ATOMIC_RELEASE);
	thread_wakeup((event_t)&ad_irq_pending);
	return 0;
}

/* Once both the window (rproc_map) and the event source (PandoraIpcc::start) are there. */
static void
ad_irq_start(void)
{
	int on = 1;
	(void)PE_parse_boot_argn("pandora_ipcc_irq", &on, sizeof(on));
	if (!ad_mapped || !ad_irq_enable || !on) {
		return;
	}
	int expect = 0;
	if (!__atomic_compare_exchange_n(&ad_irq_state, &expect, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		return;
	}
	ad_irq_enable();
	__atomic_store_n(&ad_irq_state, 2, __ATOMIC_RELEASE);
	printf("PANDORA ADSP: IPCC interrupt on: the links' thread sleeps until a doorbell\n");
}

/* PandoraIpcc.cpp: its event source exists (still disabled); ENABLE turns the line on. */
void pandora_ipcc_irq_attach(void (*enable)(void));
void
pandora_ipcc_irq_attach(void (*enable)(void))
{
	ad_irq_enable = enable;
	ad_irq_start();
}

static bool
ad_irq_on(void)
{
	return __atomic_load_n(&ad_irq_state, __ATOMIC_ACQUIRE) == 2;
}

/* Until a doorbell (the interrupt) or ms pass. */
static void
ad_wait(unsigned ms)
{
	if (__atomic_exchange_n(&ad_irq_pending, 0, __ATOMIC_ACQ_REL)) {
		return;
	}
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline((event_t)&ad_irq_pending, THREAD_UNINT, deadline);
	if (__atomic_exchange_n(&ad_irq_pending, 0, __ATOMIC_ACQ_REL)) {
		clear_wait(current_thread(), THREAD_AWAKENED);
		return;
	}
	thread_block(THREAD_CONTINUE_NULL);
	(void)__atomic_exchange_n(&ad_irq_pending, 0, __ATOMIC_ACQ_REL);
}

/* ---- SMP2P ---------------------------------------------------------------------------- */

static bool ad_handed;
static uint64_t ad_t0;                   /* main image auth_and_reset */

static void
p2p_kick(struct edge *e)
{
	ad_wr(ad_ipcc, IPCC_SEND, (e->client << 16) | IPCC_SIG_SMP2P);
}

/* The partition, what an earlier start left, our outbound item with "master-kernel", the doorbells. */
static int
p2p_setup(struct edge *e, const char *when)
{
	int rc = qcom_smem_partition(&ad_smem, QCOM_SMEM_HOST_APPS, e->pid, &e->part);
	if (rc) {
		printf("PANDORA %s: no SMEM partition apps-%s: %d\n", e->name, e->name, rc);
		return ENODEV;
	}
	smem_survey(e, when);
	volatile uint8_t *in = smem_item(e, e->item_in, NULL);
	if (in) {
		QcomSmp2p view;
		bzero(&view, sizeof(view));
		view.in = (volatile uint32_t *)in;
		int index = qcom_smp2p_check_in(view.in, 0, e->pid) ? -1 : qcom_smp2p_find_in(&view, "slave-kernel");
		printf("PANDORA %s: SMP2P from an earlier start: slave-kernel %#x\n", e->name,
		    index >= 0 ? qcom_smp2p_in_value(&view, index) : 0xffffffffu);
	}
	if ((rc = smem_alloc(e, e->item_out, QCOM_SMP2P_ITEM_BYTES))) {
		return rc;
	}
	uint32_t osize = 0;
	volatile uint8_t *out = smem_item(e, e->item_out, &osize);
	if (!out || osize < QCOM_SMP2P_ITEM_BYTES) {
		printf("PANDORA %s: SMP2P item %u is %u bytes\n", e->name, e->item_out, osize);
		return EIO;
	}
	/* As qcom_smp2p_alloc_outbound_item, an item already there is made fresh. */
	qcom_smp2p_init_out(&e->p2p, (volatile uint32_t *)out, 0, e->pid);
	int stop = qcom_smp2p_add_entry(&e->p2p, "master-kernel");
	e->out_entry = stop;
	ad_wr(ad_ipcc, IPCC_RECV_ENABLE, (e->client << 16) | IPCC_SIG_SMP2P);
	ad_wr(ad_ipcc, IPCC_RECV_ENABLE, (e->client << 16) | IPCC_SIG_GLINK);
	e->p2p_on = true;
	p2p_kick(e);
	printf("PANDORA %s: SMP2P item %u made (entry master-kernel %d), doorbells enabled\n", e->name, e->item_out,
	    stop);
	return 0;
}

static void
p2p_service(struct edge *e)
{
	if (!e->p2p_on) {
		return;
	}
	if (!e->p2p.in) {
		uint32_t size = 0;
		volatile uint8_t *in = smem_item(e, e->item_in, &size);
		if (!in) {
			return;
		}
		e->p2p.in = (volatile uint32_t *)in;
		printf("PANDORA %s: SMP2P item %u (%u bytes) there after %llu ms: check %d\n", e->name, e->item_in, size,
		    ad_ms(e->t0), qcom_smp2p_check_in(e->p2p.in, 0, e->pid));
	}
	uint32_t value = 0, changed = 0;
	if (qcom_smp2p_service(&e->p2p, "slave-kernel", &value, &changed)) {
		p2p_kick(e);
	}
	if (changed) {
		printf("PANDORA %s: slave-kernel %#x (changed %#x) after %llu ms\n", e->name, value, changed, ad_ms(e->t0));
	}
	e->slave = value;
	if (changed & value & SLAVE_FATAL) {
		smem_crash_reason(e);
	}
	if (e == &ed_adsp && (value & SLAVE_HANDOVER) && !ad_handed) {
		/* qcom_pas_handover and q6v5_handover_interrupt: the proxy votes go, the DSP holds its own now. */
		ad_handed = true;
		(void)arc_vote("lcx.lvl", 0);
		(void)arc_vote("lmx.lvl", 0);
		(void)bcm_vote("LP0", 0);
	}
}

/* ---- GLINK ------------------------------------------------------------------------------ */

/* Packets from a channel, handled after the poll (the callbacks run inside it). */
struct inbox {
	unsigned n, lost;
	struct {
		uint32_t len;
		uint8_t data[288];
	} m[6];
};

static void
inbox_put(struct inbox *b, const uint8_t *data, uint32_t len)
{
	if (b->n == 6) {
		b->lost++;
		return;
	}
	b->m[b->n].len = MIN(len, (uint32_t)sizeof(b->m[0].data));
	memcpy(b->m[b->n].data, data, b->m[b->n].len);
	b->n++;
}

static void
gl_kick(void *ctx)
{
	struct edge *e = ctx;
	ad_wr(ad_ipcc, IPCC_SEND, (e->client << 16) | IPCC_SIG_GLINK);
}

/*
 * qcom_glink_smem_register: the descriptor and our FIFO (an earlier session's are reused), our two
 * indices zeroed (our FIFO's head, the remote FIFO's tail). That assumes the remote's indices are 0
 * as well, which holds for a remote we start (the ADSP). A remote that is running already (the
 * SOCCP: attached) keeps the indices of the boot loaders' session. bat1: our FIFO's tail (the
 * SOCCP's read index) was 8, so a head of 0 left 8 - 16 bytes of room (the phone's
 * glink_smem_tx_avail keeps 16 back) and VERSION never went out; the SOCCP's 16 bytes waited for
 * a VERSION_ACK that could not go out either (DeepSeek's soccpcheck RESULT.md, checked against
 * qcom_glink_smem.ko). attached: our head goes on from the remote's read index (anything the boot
 * loaders wrote and the remote did not read is dropped) and our read index stays where the boot
 * loaders left it (what the remote sent them is read now), or skips to the remote's head when it
 * cannot be a FIFO position.
 */
static int
gl_start(struct edge *e, bool attached)
{
	uint32_t dsize = 0, tsize = 0, was[4] = {0, 0, 0, 0};
	volatile uint8_t *old = smem_item(e, AD_ITEM_GL_DESC, &dsize);
	if (old && dsize >= 16) {
		const volatile uint32_t *w = (const volatile uint32_t *)old;
		for (unsigned i = 0; i < 4; i++) {
			was[i] = w[i];
		}
		printf("PANDORA %s: GLINK descriptor from an earlier session: our FIFO tail %u head %u, theirs tail %u "
		    "head %u\n", e->name, was[0], was[1], was[2], was[3]);
	}
	int err = smem_alloc(e, AD_ITEM_GL_DESC, 32);
	if (!err) {
		err = smem_alloc(e, AD_ITEM_GL_TX, AD_GL_FIFO);
	}
	volatile uint8_t *desc = smem_item(e, AD_ITEM_GL_DESC, &dsize), *txf = smem_item(e, AD_ITEM_GL_TX, &tsize);
	printf("PANDORA %s: GLINK items %u (%u bytes) %u (%u bytes) -> %d\n", e->name, AD_ITEM_GL_DESC, dsize,
	    AD_ITEM_GL_TX, tsize, err);
	if (err || !desc || !txf || dsize < 32) {
		return err ? err : ENOENT;
	}
	/* qcom_glink_smem.c: descriptor words 0/1 our FIFO's tail/head, 2/3 the remote's. */
	volatile uint32_t *d = (volatile uint32_t *)desc;
	QcomGlinkFifo tx = {&d[1], &d[0], txf, tsize}, rx = {&d[3], &d[2], NULL, 0};
	qcom_glink_init(&e->gl, tx, rx, gl_kick, e);
	if (attached && old) {
		bool keep = was[2] < AD_GL_FIFO && !(was[2] & 7u);
		d[1] = d[0];
		d[2] = keep ? was[2] : d[3];
		__asm__ volatile ("dsb sy" ::: "memory");
		printf("PANDORA %s: GLINK goes on from the earlier session (%s): our FIFO tail %u head %u, theirs tail %u "
		    "head %u\n", e->name, keep ? "our read index kept" : "our read index unusable, their FIFO skipped",
		    d[0], d[1], d[2], d[3]);
	}
	e->gl_on = true;
	return 0;
}

static void
gl_service(struct edge *e)
{
	if (!e->gl_on) {
		return;
	}
	if (!e->gl.rx.data) {
		uint32_t size = 0;
		volatile uint8_t *rx = smem_item(e, AD_ITEM_GL_RX, &size);
		if (rx) {
			qcom_glink_set_rx(&e->gl, rx, size);
			printf("PANDORA %s: GLINK item %u (%u bytes) there after %llu ms\n", e->name, AD_ITEM_GL_RX, size,
			    ad_ms(e->t0));
		}
	}
	(void)qcom_glink_poll(&e->gl);
}

/* What changed in the link since the last report. */
static void
gl_report(struct edge *e, bool force)
{
	if (!e->gl_on) {
		return;
	}
	uint32_t bells = e->bells[0] + e->bells[1];
	if (!force && e->shown.agreed == e->gl.version_agreed && e->shown.broken == e->gl.broken &&
	    e->shown.unknown == e->gl.unknown_opens && e->shown.bells == bells) {
		return;
	}
	e->shown.agreed = e->gl.version_agreed;
	e->shown.broken = e->gl.broken;
	e->shown.unknown = e->gl.unknown_opens;
	e->shown.bells = bells;
	e->gl.unknown_names[sizeof(e->gl.unknown_names) - 1] = 0;
	printf("PANDORA %s: at %llu ms GLINK version %s%s, messages in %u out %u data %u dropped %u; opens nobody "
	    "serves %u (%s); FIFOs ours %u/%u theirs %u/%u (tail/head); doorbells SMP2P %u GLINK %u; AOP %u other %u "
	    "(last %#x); IPCC interrupt state %d, %u interrupts, %u empty\n", e->name, ad_ms(e->t0),
	    e->gl.version_agreed ? "agreed" : "not agreed", e->gl.broken ? ", LINK BROKEN" : "", e->gl.rx_messages,
	    e->gl.tx_messages, e->gl.rx_data, e->gl.dropped, e->gl.unknown_opens, e->gl.unknown_names, *e->gl.tx.tail,
	    *e->gl.tx.head, *e->gl.rx.tail, *e->gl.rx.head, e->bells[0], e->bells[1], ad_ipcc_aop, ad_ipcc_other,
	    ad_ipcc_last, ad_irq_state, ad_irq_count, ad_irq_empty);
}

/* ---- the audio DSP's GPR channel ------------------------------------------------------- */

#define GPR_INTENT       512u            /* DT qcom,gpr qcom,intents = <0x200 0x14> */
#define GPR_INTENTS      20u
#define GPR_ANSWER_MS    1000u
#define GPR_RETRY_MS     100u
#define GPR_QUERIES      100u

static QcomGlinkChannel *ad_gpr;
static uint8_t ad_gpr_buf[GPR_INTENT * GPR_INTENTS];
static GprSpfProbe ad_spf;
static bool ad_gpr_up, ad_gpr_changed, ad_spf_done;
static uint64_t ad_gpr_up_at, ad_answer_by, ad_retry_at;
static uint8_t ad_tx[GPR_HEADER_BYTES];
static uint32_t ad_tx_len;
static struct inbox ad_gpr_in;

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
	inbox_put(&ad_gpr_in, data, len);
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

static void spk_receive(const uint8_t *data, uint32_t len);

/* Channel state, the packets that came in, the timers, the query waiting to go out. */
static void
gpr_step(void)
{
	if (!ad_gpr) {
		return;
	}
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
	for (unsigned i = 0; i < ad_gpr_in.n; i++) {
		GprPacket p;
		if (!gpr_decode(ad_gpr_in.m[i].data, ad_gpr_in.m[i].len, &p) && ad_spf.answers + ad_spf.ignored < 8) {
			printf("PANDORA ADSP: GPR in: opcode %#x token %u from domain %u port %#x, %u bytes\n", p.opcode, p.token,
			    p.src_domain, p.src_port, p.size);
		}
		if (ad_gpr_up && !ad_spf_done) {
			gpr_outcome(gpr_spf_receive(&ad_spf, ad_gpr_in.m[i].data, ad_gpr_in.m[i].len));
		} else if (ad_spf_done) {
			spk_receive(ad_gpr_in.m[i].data, ad_gpr_in.m[i].len);
		}
	}
	ad_gpr_in.n = 0;
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
	if (ad_tx_len && qcom_glink_send(&ed_adsp.gl, ad_gpr, ad_tx, ad_tx_len) == QcomGlinkOk) {
		ad_tx_len = 0;
	}
}

/* ---- speaker groundwork: our memory as the DSP sees it, and the TDM bit clock ------------ */

/*
 * Pieces the speakers need whatever plays through them, run once after the APM is ready, with
 * boot-arg pandora_spk=LEVEL:
 *  1: the buffer the loader keeps below 4 GiB (ADT /chosen pandora-dsp-shm, mapped write-combined at
 *     boot) is mapped for the DSP (APM_CMD_SHARED_MEM_MAP_REGIONS, pool SHMEM8_4K, one region, as
 *     Linux's audioreach_map_memory_regions) and unmapped again. The DSP takes it as a 32-bit address
 *     with the stream index in the upper word (DT msm-audio-ion 0x1001, qcom,smmu-sid-mask 0xf); the
 *     apps SMMU passes those streams through (smmu_audio_passthrough). Skipped when either is missing.
 *  2: the TDM0 pins (GPIO 122-125 i2s0_sck/data0/data1/ws, function 1 in Linux's
 *     pinctrl-kaanapali.c, 8 mA, no pull: DT tdm0 pin groups) and the primary TDM bit clock as
 *     Android asks the PRM for it (machine_dlkm msm_common_snd_hw_params, D:\DeepSeekHarness\prmcheck:
 *     PARAM_ID_RSC_AUDIO_HW_CLK, clock 0x200 at 48 kHz x 2 slots x 32 bits, attribute 4 from the DT's
 *     tdm-clk-attribute, root 0), held 2 s and released.
 * Nothing plays: the amplifiers are not configured.
 */
#define SPK_APM_PORT          GPR_PORT_APM
#define SPK_PRM_PORT          2u        /* the PRM instance (Linux q6prm GPR_PRM_MODULE_IID) */
#define APM_CMD_MEM_MAP       0x0100100Cu
#define APM_CMD_MEM_UNMAP     0x0100100Du
#define APM_RSP_MEM_MAP       0x02001001u
#define APM_POOL_SHMEM8_4K    3u
#define PRM_CMD_REQUEST       0x0100100Fu
#define PRM_CMD_RELEASE       0x01001010u
#define PRM_RSP_REQUEST       0x02001002u
#define PRM_RSP_RELEASE       0x02001003u
#define PRM_PARAM_HW_CLK      0x0800102Cu
#define PRM_CLK_PRI_TDM_IBIT  0x200u
#define PRM_CLK_ATTR          4u        /* invert, couple no */
#define SPK_TDM_HZ            (48000u * 2u * 32u)
#define SPK_SID_INDEX         1u
#define SPK_TLMM_PA           0x0f100000u
#define SPK_TDM_GPIO0         122u
#define SPK_ANSWER_MS         1000u
#define SPK_HOLD_MS           2000u

enum {SpkOff, SpkWait, SpkMap, SpkUnmap, SpkClock, SpkHold, SpkRelease, SpkDone};

static vm_offset_t ad_shm_va;            /* the DSP buffer (smmu_survey) */
static uint64_t ad_shm_pa, ad_shm_size;
static bool ad_smmu_audio;               /* the audio streams pass the apps SMMU */

static struct {
	int level;
	unsigned state;
	uint8_t tx[128];
	uint32_t tx_len, token, handle, others;
	uint64_t answer_by, hold_until, t0;
} spk;

static void
spk_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t
spk_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* One command to the APM or the PRM; the answer is expected within SPK_ANSWER_MS. */
static void
spk_send(uint32_t port, uint32_t opcode, const uint8_t *payload, uint32_t n, unsigned state)
{
	GprPacket p;
	bzero(&p, sizeof(p));
	p.dst_domain = GPR_DOMAIN_ADSP;
	p.src_domain = GPR_DOMAIN_APPS;
	p.src_port = port;
	p.dst_port = port;
	p.token = 0x5300u + ++spk.token;
	p.opcode = opcode;
	spk.tx_len = gpr_encode(spk.tx, sizeof(spk.tx), &p, payload, n);
	spk.state = state;
	clock_interval_to_deadline(SPK_ANSWER_MS, NSEC_PER_MSEC, &spk.answer_by);
}

/* The PRM's audio hardware clock resource: request (hz) or release (hz 0) the primary TDM bit clock. */
static void
spk_clock(uint32_t hz)
{
	uint8_t pl[52];
	bzero(pl, sizeof(pl));
	/* apm_cmd_header {payload address lsw, msw, mem map handle, payload size}; then one parameter */
	spk_put32(pl + 12, hz ? 36u : 24u);
	spk_put32(pl + 16, SPK_PRM_PORT);          /* module instance */
	spk_put32(pl + 20, PRM_PARAM_HW_CLK);
	spk_put32(pl + 24, hz ? 20u : 8u);
	spk_put32(pl + 32, 1);                     /* one clock */
	spk_put32(pl + 36, PRM_CLK_PRI_TDM_IBIT);
	if (hz) {
		spk_put32(pl + 40, hz);
		spk_put32(pl + 44, PRM_CLK_ATTR);
		spk_put32(pl + 48, 0);                 /* default root */
	}
	spk_send(SPK_PRM_PORT, hz ? PRM_CMD_REQUEST : PRM_CMD_RELEASE, pl, hz ? 52u : 40u, hz ? SpkClock : SpkRelease);
}

/* TDM0's four pins to the I2S function, as the DT's tdm0 groups. */
static void
spk_pins(void)
{
	vm_offset_t t = ad_map(SPK_TLMM_PA + SPK_TDM_GPIO0 * 0x1000ull, 4 * 0x1000ull, VM_WIMG_IO);
	if (!t) {
		printf("PANDORA SPK: TLMM not mapped\n");
		return;
	}
	for (unsigned i = 0; i < 4; i++) {
		volatile uint32_t *ctl = (volatile uint32_t *)(t + i * 0x1000u);
		uint32_t old = *ctl;
		/* pull 1:0 none, function 5:2 = 1, drive 8:6 = 8 mA, output enable 9 off (the function drives) */
		uint32_t v = (old & ~0x3ffu) | (1u << 2) | ((8u / 2u - 1u) << 6);
		if (v & (1u << 11)) {
			v |= 1u << 12;                     /* an eGPIO: claim it for the TLMM, as pinctrl-msm does */
		}
		*ctl = v;
		__asm__ volatile ("dsb sy" ::: "memory");
		printf("PANDORA SPK: GPIO %u control %#x -> %#x (reads %#x)\n", SPK_TDM_GPIO0 + i, old, v, *ctl);
	}
	ml_io_unmap(t, 4 * 0x1000u);
}

static void
spk_level2(void)
{
	if (spk.level < 2) {
		spk.state = SpkDone;
		return;
	}
	spk_pins();
	spk_clock(SPK_TDM_HZ);
}

static void
spk_receive(const uint8_t *data, uint32_t len)
{
	GprPacket p;
	if (gpr_decode(data, len, &p) != GprOk) {
		return;
	}
	const uint32_t w0 = p.payload_bytes >= 4 ? spk_le32(p.payload) : 0;
	const uint32_t w1 = p.payload_bytes >= 8 ? spk_le32(p.payload + 4) : 0;
	if (spk.state == SpkOff || spk.state == SpkDone || p.token != 0x5300u + spk.token) {
		if (spk.others++ < 8) {
			printf("PANDORA SPK: GPR in: opcode %#x token %#x from port %#x, %u bytes, words %#x %#x\n", p.opcode,
			    p.token, p.src_port, p.size, w0, w1);
		}
		return;
	}
	spk.answer_by = 0;
	switch (spk.state) {
	case SpkMap:
		if (p.opcode == APM_RSP_MEM_MAP && p.payload_bytes >= 4) {
			spk.handle = w0;
			printf("PANDORA SPK: the APM mapped %#llx + %#llx (address word %#x) as handle %#x after %llu ms\n",
			    ad_shm_pa, ad_shm_size, SPK_SID_INDEX, spk.handle, ad_ms(spk.t0));
			uint8_t pl[4];
			spk_put32(pl, spk.handle);
			spk_send(SPK_APM_PORT, APM_CMD_MEM_UNMAP, pl, 4, SpkUnmap);
		} else {
			printf("PANDORA SPK: memory map refused: opcode %#x words %#x %#x\n", p.opcode, w0, w1);
			spk_level2();
		}
		break;
	case SpkUnmap:
		printf("PANDORA SPK: unmap answer opcode %#x: command %#x status %#x\n", p.opcode, w0, w1);
		spk_level2();
		break;
	case SpkClock:
		printf("PANDORA SPK: PRM clock request (%u Hz, attribute %u) answer opcode %#x: command %#x status %#x\n",
		    SPK_TDM_HZ, PRM_CLK_ATTR, p.opcode, w0, w1);
		if (p.opcode == PRM_RSP_REQUEST && w1 == 0) {
			spk.state = SpkHold;
			clock_interval_to_deadline(SPK_HOLD_MS, NSEC_PER_MSEC, &spk.hold_until);
		} else {
			spk.state = SpkDone;
		}
		break;
	case SpkRelease:
		printf("PANDORA SPK: PRM clock release answer opcode %#x: command %#x status %#x\n", p.opcode, w0, w1);
		spk.state = SpkDone;
		break;
	default:
		break;
	}
}

static void
spk_step(void)
{
	if (spk.state == SpkOff || spk.state == SpkDone || !ad_gpr) {
		return;
	}
	uint64_t now = mach_absolute_time();
	if (spk.state == SpkWait) {
		if (!ad_spf_done) {
			return;
		}
		if (ad_spf.state != SpfReady) {
			printf("PANDORA SPK: the APM never got ready: nothing to do\n");
			spk.state = SpkDone;
			return;
		}
		spk.t0 = now;
		if (ad_shm_va && ad_smmu_audio) {
			uint8_t pl[20];
			bzero(pl, sizeof(pl));
			pl[0] = APM_POOL_SHMEM8_4K;            /* u16 pool, u16 regions, u32 flags, then {lsw, msw, bytes} */
			pl[2] = 1;
			spk_put32(pl + 8, (uint32_t)ad_shm_pa);
			spk_put32(pl + 12, SPK_SID_INDEX | (uint32_t)(ad_shm_pa >> 32));
			spk_put32(pl + 16, (uint32_t)ad_shm_size);
			spk_send(SPK_APM_PORT, APM_CMD_MEM_MAP, pl, sizeof(pl), SpkMap);
		} else {
			printf("PANDORA SPK: no %s: memory map skipped\n", ad_shm_va ? "SMMU pass-through" : "DSP buffer");
			spk_level2();
		}
	}
	if (spk.state == SpkHold && now >= spk.hold_until) {
		spk_clock(0);
	}
	if (spk.answer_by && now >= spk.answer_by) {
		spk.answer_by = 0;
		spk.tx_len = 0;
		printf("PANDORA SPK: no answer in state %u\n", spk.state);
		if (spk.state == SpkMap || spk.state == SpkUnmap) {
			spk_level2();
		} else if (spk.state == SpkClock) {
			spk_clock(0);                          /* in case it was taken */
		} else {
			spk.state = SpkDone;
		}
	}
	if (spk.tx_len && qcom_glink_send(&ed_adsp.gl, ad_gpr, spk.tx, spk.tx_len) == QcomGlinkOk) {
		spk.tx_len = 0;
	}
}

/* ---- the battery: the SOCCP's side of Xiaomi's battery manager over PMIC GLINK --------- */

/*
 * Channel "PMIC_RTR_SOCCP_APPS" (DT pmic-glink, qcom,subsys-name "soccp"). The battery client Android
 * runs on this phone is Xiaomi's MCA (mca_adsp_glink.ko under DT mca,adsp_glink; the classic
 * qti_battery_charger node is disabled), so its messages are the ones sent: a read is 276 bytes
 * {owner 0x800a, type 1, opcode 1, property, sequence, 256 bytes of zeros} (mca_adsp_glink_read_prop)
 * and is answered with exactly 280 bytes {owner, type 1, opcode 1, property, result, sequence, 256
 * bytes of data} (mca_adsp_glink_callback: length 0x118, result 0, sequence as sent); type 2 is a
 * notification {header, number, 256 bytes}. bat2 asked with the classic 24-byte opcode 0x30 reads:
 * each came back as a 280-byte answer the old code did not take. The properties are the 4-byte ones
 * mca_qcom_subpmic_proxy reads. The percentage is not among them: Android reads the bq27z561 gauge on
 * the shared I2C bus (DT i2c@a94000) itself, and so do we (pandora_amp.c). Until the units are
 * settled against Android's readings, only bus_volt goes to the battery service (charger connected);
 * the rest are logged: a pass after the channel comes up, then every 20 s and after each notification.
 * Only reads go out; charging stays the SOCCP's.
 */
#define BM_OWNER         0x800au
#define BM_TYPE_REQUEST  1u
#define BM_TYPE_NOTIFY   2u
#define BM_OP_READ       1u
#define BM_REQUEST_LEN   0x114u
#define BM_ANSWER_LEN    0x118u
#define BM_ANSWER_MS     1000u
#define BM_REFRESH_MS    20000u
#define BM_INTENT        1024u
#define BM_INTENTS       8u

/* mca_qcom_subpmic_proxy: the getter that reads each one. */
static const struct {
	uint32_t prop;
	const char *name;
} bm_props[] = {
	{0x2027, "pack_vbat"}, {0x2028, "pack_ibat"}, {0x202b, "pack_tbat"}, {0x2001, "chg_status"},
	{0x2002, "chg_type"}, {0x0001, "usb_type"}, {0x20002, "bus_volt"}, {0x0004, "bus_curr"},
	{0x0003, "usb_sns_volt"}, {0x200c, "vsys_volt"}, {0x2008, "input_current_limit"},
};
#define BM_PROPS (sizeof(bm_props) / sizeof(bm_props[0]))

static struct {
	QcomGlinkChannel *ch;
	bool up, changed, open_forced;
	uint8_t storage[BM_INTENT * BM_INTENTS];
	struct inbox in;
	uint8_t tx[BM_REQUEST_LEN];
	bool pending, inflight;                /* a request built and not sent yet; sent, no answer yet */
	uint32_t prop, seq;                    /* that request */
	unsigned step;                         /* index into bm_props */
	bool reading;                          /* a pass is under way */
	uint64_t answer_by, refresh_at, open_by;
	int32_t value[BM_PROPS];
	bool ok[BM_PROPS];                     /* value answered without error in this pass */
	uint32_t passes, answers, timeouts, notes, others;
	char line[640];
} bm;

LCK_GRP_DECLARE(ad_batt_grp, "pandora_battery");
LCK_MTX_DECLARE(ad_batt_lock, &ad_batt_grp);
static struct pandora_battery ad_batt;   /* what pandora_battery_get hands out (nothing valid yet) */
#define BM_BUS_VOLT      0x20002u        /* bus_volt, µV (spk1: about 5 000 000 on the PC's USB) */
static int32_t ad_bus_uv = -1;           /* the last MCA pass's bus voltage, -1 unknown */
static uint64_t ad_bus_at;               /* when it was taken */

static void
bm_state(void *ctx, QcomGlinkChannel *ch, int up)
{
	(void)ctx;
	(void)ch;
	bm.up = up != 0;
	bm.changed = true;
}

static void
bm_receive(void *ctx, QcomGlinkChannel *ch, const uint8_t *data, uint32_t len)
{
	(void)ctx;
	(void)ch;
	inbox_put(&bm.in, data, len);
}

static uint32_t
bm_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void
bm_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* The next read of the pass; false when the pass is over. */
static bool
bm_next(void)
{
	if (bm.step >= BM_PROPS) {
		return false;
	}
	bzero(bm.tx, sizeof(bm.tx));
	bm.prop = bm_props[bm.step].prop;
	bm.seq++;
	bm_put32(bm.tx, BM_OWNER);
	bm_put32(bm.tx + 4, BM_TYPE_REQUEST);
	bm_put32(bm.tx + 8, BM_OP_READ);
	bm_put32(bm.tx + 12, bm.prop);
	bm_put32(bm.tx + 16, bm.seq);
	bm.pending = true;
	bm.inflight = false;
	clock_interval_to_deadline(BM_ANSWER_MS, NSEC_PER_MSEC, &bm.answer_by);
	return true;
}

static void
bm_pass_start(void)
{
	bm.reading = true;
	bm.step = 0;
	bm.line[0] = 0;
	(void)bm_next();
}

/* One answer or a notification. */
static void
bm_handle(const uint8_t *m, uint32_t len)
{
	if (len < 16) {
		bm.others++;
		return;
	}
	uint32_t owner = bm_le32(m), type = bm_le32(m + 4), op = bm_le32(m + 8);
	if (owner == BM_OWNER && type == BM_TYPE_NOTIFY) {
		if (bm.notes++ < 8) {
			printf("PANDORA SOCCP: notification opcode %#x number %#x first word %#x (%u bytes)\n", op, bm_le32(m + 12),
			    len >= 20 ? bm_le32(m + 16) : 0, len);
		}
		bm.refresh_at = mach_absolute_time();  /* something changed: read again now */
		return;
	}
	if (owner != BM_OWNER || type != BM_TYPE_REQUEST || len != BM_ANSWER_LEN || !(bm.pending || bm.inflight) ||
	    bm_le32(m + 12) != bm.prop || bm_le32(m + 20) != bm.seq) {
		if (bm.others++ < 8) {
			printf("PANDORA SOCCP: message owner %#x type %u opcode %#x, %u bytes, property %#x result %#x sequence %u "
			    "(waiting for %#x, %u)\n", owner, type, op, len, bm_le32(m + 12), len >= 20 ? bm_le32(m + 16) : 0,
			    len >= 24 ? bm_le32(m + 20) : 0, bm.prop, bm.seq);
		}
		return;
	}
	uint32_t result = bm_le32(m + 16);
	int32_t value = (int32_t)bm_le32(m + 24);
	char one[48];
	bm.answers++;
	bm.value[bm.step] = value;
	bm.ok[bm.step] = result == 0;
	if (result) {
		snprintf(one, sizeof(one), " %s=err%#x", bm_props[bm.step].name, result);
	} else {
		snprintf(one, sizeof(one), " %s=%d", bm_props[bm.step].name, value);
	}
	strlcat(bm.line, one, sizeof(bm.line));
	bm.pending = bm.inflight = false;
	bm.answer_by = 0;
	bm.step++;
}

static void
bm_step(void)
{
	if (!bm.ch) {
		return;
	}
	uint64_t now = mach_absolute_time();
	if (bm.changed) {
		bm.changed = false;
		printf("PANDORA SOCCP: channel PMIC_RTR_SOCCP_APPS %s after %llu ms\n", bm.up ? "up" : "down",
		    ad_ms(ed_soccp.t0));
		bm.pending = bm.inflight = bm.reading = false;
		bm.answer_by = bm.refresh_at = 0;
		if (bm.up) {
			bm_pass_start();
		}
	}
	/* Nobody opened the channel: open it ourselves (the library's own fields for an initiator). */
	if (!bm.up && !bm.open_forced && bm.open_by && now >= bm.open_by && ed_soccp.gl.version_agreed &&
	    !bm.ch->remote_open) {
		bm.open_forced = true;
		bm.ch->initiator = 1;
		bm.ch->open_sent = 0;
		printf("PANDORA SOCCP: the SOCCP did not open PMIC_RTR_SOCCP_APPS: opening it\n");
	}
	for (unsigned i = 0; i < bm.in.n; i++) {
		bm_handle(bm.in.m[i].data, bm.in.m[i].len);
	}
	bm.in.n = 0;
	if (!bm.up) {
		return;
	}
	if (bm.answer_by && now >= bm.answer_by) {
		char one[40];
		bm.timeouts++;
		bm.ok[bm.step] = false;
		snprintf(one, sizeof(one), " %s=?", bm_props[bm.step].name);
		strlcat(bm.line, one, sizeof(bm.line));
		bm.pending = bm.inflight = false;
		bm.answer_by = 0;
		bm.step++;
	}
	if (bm.reading && !bm.pending && !bm.inflight && !bm_next()) {
		bm.reading = false;
		bm.passes++;
		printf("PANDORA SOCCP: MCA pass %u at %llu ms:%s (answers %u, timeouts %u)\n", bm.passes, ad_ms(ed_soccp.t0),
		    bm.line, bm.answers, bm.timeouts);
		for (unsigned i = 0; i < BM_PROPS; i++) {
			if (bm_props[i].prop == BM_BUS_VOLT) {
				lck_mtx_lock(&ad_batt_lock);
				ad_bus_uv = bm.ok[i] ? bm.value[i] : -1;
				ad_bus_at = now;
				lck_mtx_unlock(&ad_batt_lock);
			}
		}
		clock_interval_to_deadline(BM_REFRESH_MS, NSEC_PER_MSEC, &bm.refresh_at);
	}
	if (!bm.reading && bm.refresh_at && now >= bm.refresh_at) {
		bm.refresh_at = 0;
		bm_pass_start();
	}
	if (bm.pending && qcom_glink_send(&ed_soccp.gl, bm.ch, bm.tx, sizeof(bm.tx)) == QcomGlinkOk) {
		/* Sent; the answer timer has run since the request was built (waiting for an intent counts). */
		bm.pending = false;
		bm.inflight = true;
	}
}

/*
 * For iokit/Kernel/arm/SweetBattery.cpp: the latest readings; false when there are none to use.
 * The battery itself is the bq27z561 gauge's (bsd/dev/pandora_amp.c, pandora_gauge=3): its
 * RelativeStateOfCharge as the percentage, voltage, current (positive while charging), temperature,
 * the mAh figures and cycles. Android's battery psy (mca_business_battery batt_psy_get_prop, battui
 * RESULT §3.1, §5): not online is discharging; online with the percentage at 100 and charging done
 * (BatteryStatus FC, bit 5) is full; otherwise charging. Temperature is 0x06 in 0.1 K less 2730. A
 * charger counts as online when the SOCCP's bus_volt of the last 60 s is above 4 V (pandora_battery=1),
 * without one when the gauge sees charge current. Android shows a percentage a point or two above
 * 0x2c while charging (battui §2.2); that filter is not known yet.
 */
#define AD_BUS_FRESH_MS  60000u
#define AD_BUS_ON_UV     4000000

bool
pandora_battery_get(struct pandora_battery *b, size_t size)
{
	if (size != sizeof(*b)) {
		return false;
	}
	struct pandora_gauge g;
	const bool gauge = pandora_gauge_get(&g, sizeof(g));
	lck_mtx_lock(&ad_batt_lock);
	*b = ad_batt;
	const int32_t bus_uv = ad_bus_uv;
	const uint64_t bus_at = ad_bus_at;
	lck_mtx_unlock(&ad_batt_lock);
	if (gauge) {
		b->valid = b->present = 1;
		b->percent = g.soc < 0 ? 0 : g.soc > 100 ? 100 : g.soc;
		b->voltage_mv = g.voltage_mv;
		b->current_ma = g.current_ma;
		b->temperature = (g.temperature_dk - 2730) * 10;
		b->temperature_ok = g.temperature_dk > 2330 && g.temperature_dk < 3730;   /* -40 to 100 degrees */
		b->raw_mah = g.remaining_mah;
		b->full_mah = g.full_mah;
		b->design_mah = g.design_mah;
		b->cycles = g.cycles;
		const bool bus_known = bus_at && bus_uv >= 0 && ad_ms(bus_at) < AD_BUS_FRESH_MS;
		b->external = bus_known ? bus_uv > AD_BUS_ON_UV : g.current_ma > 0;
		b->fully_charged = b->external && b->percent == 100 && (g.status & 0x20) != 0;
		b->charging = b->external && !b->fully_charged;
		b->time_to_full_min = b->time_to_empty_min = -1;
	}
	return b->valid != 0;
}

/*
 * The phone's adsp_attach (qcom_q6v5_pas.ko) checks the remote is alive before GLINK starts:
 * ping_subsystem (qcom_q6v5.ko) sets the "ping" bit of master-kernel (DT qcom,smem-states: bit 8;
 * the whole entry is written, mask ~0), waits up to 125 jiffies for the "pong" interrupt
 * (slave-kernel bit 9) and clears the ping bit. The phone panics without a pong; here it is logged.
 */
#define SO_PING_BIT      (1u << 8)
#define SO_PONG_BIT      (1u << 9)

static void
soccp_ping(struct edge *e)
{
	uint32_t before = e->slave;
	uint64_t t = mach_absolute_time(), until;
	if (qcom_smp2p_update(&e->p2p, e->out_entry, ~0u, SO_PING_BIT)) {
		p2p_kick(e);
	}
	clock_interval_to_deadline(1500, NSEC_PER_MSEC, &until);
	while (!((e->slave ^ before) & SO_PONG_BIT) && mach_absolute_time() < until) {
		ad_sleep(1);
		ipcc_poll();
		p2p_service(e);
	}
	bool pong = ((e->slave ^ before) & SO_PONG_BIT) != 0;
	if (qcom_smp2p_update(&e->p2p, e->out_entry, SO_PING_BIT, 0)) {
		p2p_kick(e);
	}
	printf("PANDORA SOCCP: ping -> %s after %llu ms (slave-kernel %#x before, %#x now)\n",
	    pong ? "pong" : "NO pong", ad_ms(t), before, e->slave);
}

/* The SOCCP is running already: its partition, our SMP2P item, the ping, GLINK and the battery channel. */
static void
soccp_attach(void)
{
	struct edge *e = &ed_soccp;
	e->t0 = mach_absolute_time();
	if (p2p_setup(e, "at attach")) {
		return;
	}
	p2p_service(e);
	/* qcom_pas_attach: a running SOCCP shows ready and no fatal or stop. */
	printf("PANDORA SOCCP: slave-kernel %#x at attach (%s)\n", e->slave,
	    (e->slave & SLAVE_FATAL) ? "FATAL" : (e->slave & SLAVE_READY) ? "ready" : "not ready");
	soccp_ping(e);
	if (gl_start(e, true)) {
		return;
	}
	int open_now = 0;
	(void)PE_parse_boot_argn("pandora_battery_open", &open_now, sizeof(open_now));
	static const QcomGlinkClient client = {bm_state, bm_receive};
	bm.ch = qcom_glink_channel(&e->gl, "PMIC_RTR_SOCCP_APPS", BM_INTENT, BM_INTENTS, bm.storage, &client, NULL,
	    open_now);
	clock_interval_to_deadline(5000, NSEC_PER_MSEC, &bm.open_by);
	gl_service(e);
	gl_report(e, true);
}

/* ---- the audio DSP's start --------------------------------------------------------------- */

/* After the main image's reset: ready (or fatal) within 5 s, then GLINK and the GPR channel. */
static bool
adsp_ready(int open_gpr)
{
	/* qcom_q6v5_wait_for_start: 5 s; pandora_adsp_wait=MS waits longer, to tell slow from dead. */
	uint64_t until;
	unsigned wait_ms = 5000;
	(void)PE_parse_boot_argn("pandora_adsp_wait", &wait_ms, sizeof(wait_ms));
	clock_interval_to_deadline(wait_ms, NSEC_PER_MSEC, &until);
	while (!(ed_adsp.slave & (SLAVE_READY | SLAVE_FATAL)) && mach_absolute_time() < until) {
		ipcc_poll();
		p2p_service(&ed_adsp);
		ad_sleep(1);
	}
	smem_survey(&ed_adsp, "after the wait");
	if (!(ed_adsp.slave & SLAVE_READY) || (ed_adsp.slave & SLAVE_FATAL)) {
		printf("PANDORA ADSP: not ready after %llu ms (slave-kernel %#x)\n", ad_ms(ad_t0), ed_adsp.slave);
		smem_crash_reason(&ed_adsp);
		return false;
	}
	printf("PANDORA ADSP: ready after %llu ms\n", ad_ms(ad_t0));
	if (gl_start(&ed_adsp, false)) {
		return false;
	}
	static const QcomGlinkClient client = {gpr_state, gpr_receive};
	ad_gpr = qcom_glink_channel(&ed_adsp.gl, "adsp_apps", GPR_INTENT, GPR_INTENTS, ad_gpr_buf, &client, NULL,
	    open_gpr);
	return ad_gpr != NULL;
}

static void
adsp_boot(void)
{
	int err;
	uint64_t b_dtb = 0, b_main = 0;
	struct scm_res r;
	int open_gpr = 0;
	(void)PE_parse_boot_argn("pandora_adsp_gpr", &open_gpr, sizeof(open_gpr));
	char src[8] = "";
	(void)PE_parse_boot_arg_str("pandora_adsp_fw", src, sizeof(src));
	bool part = strcmp(src, "pack") != 0 && fw_mount() == 0;
	if (!part) {
		printf("PANDORA ADSP: the ADSP images from the pinned pack\n");
		if ((err = ad_nfiles ? 0 : fw_dir())) {
			return;
		}
	}
	ed_adsp.t0 = mach_absolute_time();
	if ((err = p2p_setup(&ed_adsp, "before the start"))) {
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
	if ((err = pas_load("adsp_dtb", part, AD_PAS_DTB, AD_DTB_PA, AD_DTB_SIZE, &b_dtb)) ||
	    (err = pas_auth(AD_PAS_DTB))) {
		/* Go on with the main image anyway: its answer tells more. */
		printf("PANDORA ADSP: DTB image failed %d\n", err);
	}
	if ((err = pas_load("adsp", part, AD_PAS_MAIN, AD_MEM_PA, AD_MEM_SIZE, &b_main))) {
		printf("PANDORA ADSP: main image failed %d\n", err);
		return;
	}
	ad_t0 = ed_adsp.t0 = mach_absolute_time();
	if ((err = pas_auth(AD_PAS_MAIN))) {
		printf("PANDORA ADSP: main image failed %d\n", err);
		return;
	}
	printf("PANDORA ADSP: ADSP released from reset\n");
	if (adsp_ready(open_gpr == 2)) {
		return;
	}
	/* The DSP did not come up: stop it, as qcom_pas_start's error path. */
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

/* ---- the apps SMMU and the buffer the audio DSP reads ------------------------------------ */

/*
 * The DSP reads what the apps hand it through the apps SMMU (DT apps-smmu@15000000) on its audio
 * streams: qcom,msm-audio-ion iommus <0x1001 0x80> and <0x1041 0x20>, so 0x1001/0x1081 and
 * 0x1041/0x1061. bat2 (0233, read only): unmatched streams fault (CR0 USFCFG), none of these streams
 * has a group, 109 of 116 groups are free, the last bank (80) is unconfigured. First the groups the
 * boot loaders left, each logged; then, with pandora_spk, smmu_audio_passthrough; then the buffer.
 */

/*
 * The audio streams through the SMMU without translation, as Linux's arm-smmu-qcom.c does for bypass
 * streams: qcom_smmu_cfg_probe asks whether this firmware keeps a BYPASS S2CR (written to the last
 * S2CR, read back; here put back as well); where the firmware turns bypass into fault, the last context
 * bank becomes a bypass bank (SCTLR 0: translation off; CBAR type stage 1 translation with stage 2
 * bypass) and qcom_smmu_write_s2cr sends bypass streams to it as translated ones. One group per DT
 * entry, on free groups, S2CR before SMR (arm_smmu_write_sme). Nothing is written when a group in use
 * overlaps these streams, points at the last bank, or the last group is in use.
 */
static bool
smmu_audio_passthrough(vm_offset_t s, unsigned nsmr, unsigned nbanks, uint32_t page, uint32_t numpage)
{
#define SR(o) (*(volatile uint32_t *)(s + (o)))
	static const uint16_t ids[2] = {0x1001, 0x1041}, masks[2] = {0x80, 0x20};
	const unsigned last = nbanks - 1, probe = nsmr - 1;
	for (unsigned i = 0; i < nsmr; i++) {
		const uint32_t smr = SR(0x800 + 4 * i), s2cr = SR(0xc00 + 4 * i);
		if (!(smr & 0x80000000u)) {
			continue;
		}
		const uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
		for (unsigned k = 0; k < 2; k++) {
			if (((id ^ ids[k]) & ~(mask | masks[k]) & 0x7fff) == 0) {
				printf("PANDORA SMMU: group %u (id %#x mask %#x) overlaps the audio streams %#x/%#x: not written\n", i, id,
				    mask, ids[k], masks[k]);
				return false;
			}
		}
		if (((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) == last) {
			printf("PANDORA SMMU: group %u uses the last bank: not written\n", i);
			return false;
		}
	}
	if (SR(0x800 + 4 * probe) & 0x80000000u) {
		printf("PANDORA SMMU: the last group is in use: not written\n");
		return false;
	}
	const uint32_t was = SR(0xc00 + 4 * probe);
	SR(0xc00 + 4 * probe) = (1u << 16) | 0xffu;                /* type bypass */
	__asm__ volatile ("dsb sy" ::: "memory");
	const uint32_t back = SR(0xc00 + 4 * probe);
	SR(0xc00 + 4 * probe) = was;
	__asm__ volatile ("dsb sy" ::: "memory");
	const bool quirk = ((back >> 16) & 3) != 1;
	printf("PANDORA SMMU: a bypass S2CR reads back %08x: %s (S2CR %u back to %08x, reads %08x)\n", back,
	    quirk ? "the firmware turns bypass into something else, so the last bank passes the streams" : "bypass is kept",
	    probe, was, SR(0xc00 + 4 * probe));
	uint32_t s2cr = (1u << 16) | 0xffu;
	if (quirk) {
		const uint32_t cb = (numpage + last) * page;
		SR(cb) = 0;                                            /* SCTLR: no translation */
		SR(page + 4 * last) = 1u << 16;                        /* CBAR: stage 1 translation, stage 2 bypass */
		__asm__ volatile ("dsb sy" ::: "memory");
		printf("PANDORA SMMU: bank %u: sctlr %08x CBAR %08x CBA2R %08x\n", last, SR(cb), SR(page + 4 * last),
		    SR(page + 0x800 + 4 * last));
		s2cr = last;                                           /* type translate, that bank */
	}
	unsigned i = 0, done = 0;
	for (unsigned k = 0; k < 2; k++) {
		while (i < probe && (SR(0x800 + 4 * i) & 0x80000000u)) {
			i++;
		}
		if (i >= probe) {
			printf("PANDORA SMMU: no free group for %#x\n", ids[k]);
			break;
		}
		SR(0xc00 + 4 * i) = s2cr;
		__asm__ volatile ("dsb sy" ::: "memory");
		SR(0x800 + 4 * i) = 0x80000000u | (uint32_t)masks[k] << 16 | ids[k];
		__asm__ volatile ("dsb sy" ::: "memory");
		printf("PANDORA SMMU: group %u for streams %#x mask %#x: SMR %08x S2CR %08x\n", i, ids[k], masks[k],
		    SR(0x800 + 4 * i), SR(0xc00 + 4 * i));
		done++;
		i++;
	}
	return done == 2;
#undef SR
}

/* The buffer the loader keeps below 4 GiB for the DSP (mkadt_pandora.py: /chosen pandora-dsp-shm). */
static void
ad_shm_setup(void)
{
	DTEntry chosen;
	const uint64_t *r;
	unsigned int size;
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "pandora-dsp-shm", (void const **)&r, &size) != kSuccess || size != 16) {
		printf("PANDORA SMMU: no pandora-dsp-shm in the ADT: no DSP buffer\n");
		return;
	}
	if (!r[1] || (r[0] | r[1]) & 0xfff || r[0] + r[1] > (1ull << 32)) {
		printf("PANDORA SMMU: pandora-dsp-shm %#llx + %#llx is not 4 KiB pages below 4 GiB: no DSP buffer\n", r[0], r[1]);
		return;
	}
	vm_offset_t va = ad_map(r[0], r[1], VM_WIMG_WCOMB);
	if (!va) {
		printf("PANDORA SMMU: DSP buffer %#llx + %#llx not mapped\n", r[0], r[1]);
		return;
	}
	bzero((void *)va, (size_t)r[1]);
	ad_shm_va = va;
	ad_shm_pa = r[0];
	ad_shm_size = r[1];
	printf("PANDORA SMMU: DSP buffer %#llx + %#llx, mapped write-combined\n", ad_shm_pa, ad_shm_size);
}

static void
smmu_survey(bool write)
{
	vm_offset_t s = ad_map(0x15000000u, 0x100000u, VM_WIMG_IO);
	if (!s) {
		printf("PANDORA SMMU: not mapped\n");
		return;
	}
#define SR(o) (*(volatile uint32_t *)(s + (o)))
	const uint32_t cr0 = SR(0), id0 = SR(0x20), id1 = SR(0x24), id2 = SR(0x28);
	const unsigned nsmr = id0 & 0xff, nbanks = id1 & 0xff;
	const uint32_t page = (id1 & (1u << 31)) ? 0x10000u : 0x1000u, numpage = 1u << (((id1 >> 28) & 7) + 1);
	printf("PANDORA SMMU: cr0 %08x (client port faults unmatched streams: %s) idr0 %08x idr1 %08x idr2 %08x: %u "
	    "stream groups, %u banks, page %#x, %u global pages\n", cr0, (cr0 & (1u << 10)) ? "yes" : "no", id0, id1, id2,
	    nsmr, nbanks, page, numpage);
	if ((numpage + nbanks) * page > 0x100000u || nsmr > 128 || nsmr < 2 || !nbanks) {
		printf("PANDORA SMMU: geometry outside the window: no more reads\n");
		ml_io_unmap(s, 0x100000u);
		return;
	}
	unsigned free_groups = 0;
	for (unsigned i = 0; i < nsmr; i++) {
		const uint32_t smr = SR(0x800 + 4 * i);
		if (!(smr & 0x80000000u)) {
			free_groups++;
			continue;
		}
		const uint32_t s2cr = SR(0xc00 + 4 * i), bank = s2cr & 0xff;
		const bool cb = ((s2cr >> 16) & 3) == 0 && bank < nbanks;
		/* The bank's registers only for a translating S2CR (0 otherwise). */
		printf("PANDORA SMMU: group %u: id %#x mask %#x, S2CR %08x type %u bank %u: sctlr %08x CBAR %08x CBA2R %08x\n", i,
		    smr & 0x7fff, (smr >> 16) & 0x7fff, s2cr, (s2cr >> 16) & 3, bank, cb ? SR((numpage + bank) * page) : 0,
		    cb ? SR(page + 4 * bank) : 0, cb ? SR(page + 0x800 + 4 * bank) : 0);
	}
	const unsigned last = nbanks - 1;
	printf("PANDORA SMMU: %u of %u groups free; bank %u (the last): sctlr %08x CBAR %08x CBA2R %08x\n", free_groups, nsmr,
	    last, SR((numpage + last) * page), SR(page + 4 * last), SR(page + 0x800 + 4 * last));
#undef SR
	if (write) {
		ad_smmu_audio = smmu_audio_passthrough(s, nsmr, nbanks, page, numpage);
	}
	ml_io_unmap(s, 0x100000u);
	ad_shm_setup();
}

/* ---- the thread ------------------------------------------------------------------------- */

/*
 * Boot-args: pandora_adsp (1 TrustZone queries, 2 also start the DSP pandora_adsp_delay s after
 * the thread starts, default 20) and pandora_battery=1 (attach to the SOCCP and read the battery,
 * pandora_battery_delay s after the start, default 5), pandora_spk=LEVEL (the speaker groundwork
 * above, after the APM is ready; level 1 also writes the apps SMMU's audio pass-through at the
 * start). The links are serviced every 1 ms for 30 s after a link starts, then every 50 ms; with the
 * IPCC interrupt the thread sleeps until a doorbell or a timer.
 */
static void
adsp_thread(void *arg, wait_result_t result)
{
	struct scm_res r;
	int mode = 0, battery = 0;
	unsigned delay_s = 20, bdelay_s = 5;
	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("pandora_adsp", &mode, sizeof(mode));
	(void)PE_parse_boot_argn("pandora_adsp_delay", &delay_s, sizeof(delay_s));
	(void)PE_parse_boot_argn("pandora_battery", &battery, sizeof(battery));
	(void)PE_parse_boot_argn("pandora_battery_delay", &bdelay_s, sizeof(bdelay_s));
	(void)PE_parse_boot_argn("pandora_spk", &spk.level, sizeof(spk.level));
	if (mode < 2) {
		spk.level = 0;                     /* the speakers need the DSP */
	}
	spk.state = spk.level > 0 ? SpkWait : SpkOff;
	if (mode) {
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
	}
	if (mode < 2 && !battery) {
		printf("PANDORA ADSP: done\n");
		(void)thread_terminate(current_thread());
		thread_block(THREAD_CONTINUE_NULL);
	}
	uint64_t adsp_at, batt_at, fast_until, next_report;
	clock_interval_to_deadline(delay_s * 1000u, NSEC_PER_MSEC, &adsp_at);
	clock_interval_to_deadline(bdelay_s * 1000u, NSEC_PER_MSEC, &batt_at);
	clock_interval_to_deadline(60000, NSEC_PER_MSEC, &fast_until);
	clock_interval_to_deadline(5000, NSEC_PER_MSEC, &next_report);
	bool adsp_done = mode < 2, batt_done = !battery;
	if (rproc_map()) {
		adsp_done = batt_done = true;
	} else {
		ad_irq_start();
		smmu_survey(spk.level > 0);
	}
	for (;;) {
		uint64_t now = mach_absolute_time();
		if (!batt_done && now >= batt_at) {
			batt_done = true;
			soccp_attach();
			clock_interval_to_deadline(30000, NSEC_PER_MSEC, &fast_until);
		}
		if (!adsp_done && now >= adsp_at) {
			adsp_done = true;
			adsp_boot();
			clock_interval_to_deadline(30000, NSEC_PER_MSEC, &fast_until);
		}
		ipcc_poll();
		for (unsigned i = 0; i < 2; i++) {
			p2p_service(ad_edges[i]);
			gl_service(ad_edges[i]);
		}
		gpr_step();
		spk_step();
		bm_step();
		now = mach_absolute_time();
		for (unsigned i = 0; i < 2; i++) {
			gl_report(ad_edges[i], now >= next_report);
		}
		if (now >= next_report) {
			clock_interval_to_deadline(now < fast_until ? 5000 : 60000, NSEC_PER_MSEC, &next_report);
		}
		/* With the interrupt a doorbell wakes the thread; the timeout serves the request timers. */
		if (ad_irq_on()) {
			ad_wait(now < fast_until ? 20 : 500);
		} else {
			ad_sleep(now < fast_until ? 1 : 50);
		}
	}
}

void
pandora_adsp_init(void)
{
	thread_t thread;
	int on = 0, battery = 0;
	(void)PE_parse_boot_argn("pandora_adsp", &on, sizeof(on));
	(void)PE_parse_boot_argn("pandora_battery", &battery, sizeof(battery));
	if (!on && !battery) {
		return;
	}
	if (kernel_thread_start(adsp_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */

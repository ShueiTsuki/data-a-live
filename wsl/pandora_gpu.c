/*
 * PANDORA Adreno 840 bring-up (docs/GPU_PLAN.md, docs/GPU_PORT_MAP.md), boot-arg pandora_gpu=N.
 *
 * Level 1 (patch 0162): read only, nothing is written. A block is read only when the one it
 * hangs off is powered and clocked, because reading GPUCC, the GPU SMMU or the GMU while
 * their supply or clock is off can stall the bus and reset the phone:
 *   GCC (always on)
 *   -> GPUCC, if gcc_gpu_cfg_ahb is enabled
 *   -> GPU SMMU, if the CX GDSC is on and the SMMU vote clock runs (one SMR/S2CR at a time)
 *   -> GMU state, if the CX GDSC is on and gpu_cc_ahb and gpu_cc_cx_gmu run.
 * cmd-db (shared memory, no clock) gives the RPMh resources the GMU votes will use.
 * Before each risky read a line goes to the log and the thread waits for the log thread
 * (every 3 s) to put it on disk, so a reset still shows how far it got.
 *
 * Level 2 (patch 0163): after the level 1 reads, power the CX side the way Linux gdsc.c and
 * clk-branch.c do (gpu1 log: fastboot leaves the CX GDSC collapsed, wait values and
 * RETAIN_FF already set): clear SW_COLLAPSE in the CX GDSCR and poll PWR_ON in its
 * gds_hw_ctrl; enable the SMMU vote clock and gpu_cc_ahb and poll CLK_OFF; then the level 1
 * SMMU and GMU reads again under the same gating. The GMU clocks stay off (level 3).
 * (gpu2 log: CX on, all 24 SMRs free, GMU held in reset and idle, GMU clock on XO.)
 *
 * Level 3 (patch 0164): what Linux and KGSL gen8 do between the GMU clocks and the GMU
 * boot, without PLL0 (the GMU keeps its 19.2 MHz XO clock for now): the GPU votes on
 * GPLL0 (GCC 0x52000 bits 15/16), then SCM GPU_INIT_REGS (SIP svc 0x28 cmd 1, one
 * argument: ALWAYS_EN | TSENSE_EN = 0x9, bits from include/linux/firmware/qcom/qcom_scm.h),
 * after asking whether the call exists; then GPUCC and the GMU state are read again.
 * (gpu3 log: the call exists and returns 0.)
 *
 * Level 4 (patch 0165, "4a"): the GMU's own SMMU context on the GPU SMMU, the GMU still held
 * in reset. Five physically contiguous buffers at Linux's fixed GMU addresses (a6xx_gmu_init:
 * icache 0x4000 16 MiB-16 KiB, dummy 0x60000000, log and hfi after it, debug 0x60400000),
 * native 4 KiB stage-1 tables built by UniXNU's ArmSmmu4K (MIT, bsd/dev/adreno/), ACTLR 0x32b
 * from the phone's DT for the GMU stream, the context programmed, read back and enabled by
 * ArmSmmu4K, then stream ID 5 routed to it through a free stream-match slot.
 * (gpu4 log: context enabled and read back, slot 2 = SID 5 -> bank 1, no fault.)
 *
 * Level 5 (patch 0166, "4b"): the GMU cold start of Linux a6xx_gmu_fw_start() on the a8xx
 * path (drivers/gpu/drm/msm/adreno/a6xx_gmu.c), checked line by line against that source:
 * FAL veto, firmware blocks (gen80200_gmu.bin from the pinned pack) into ITCM/DTCM registers
 * and the icache/dummy buffers, HFI queue table (a6xx_hfi_init), FW_INIT_RESULT/BOOT_CONFIG/
 * HFI table/AHB fence/CM3_CFG/GENERAL_10 chip id/GENERAL_8 log, a840 GBIF (a6xx_catalog.c),
 * MRC_GBIF_QOS_CTRL, power config, then a6xx_gmu_start (reset release, FW_INIT_RESULT poll)
 * and a6xx_gmu_hfi_start (HFI_CTRL_INIT, HFI_CTRL_STATUS poll). No HFI message is sent yet.
 * (gpu5 log: FW_INIT_RESULT 0x100, HFI ready, the GMU writes its log buffer.)
 *
 * Level 6 (patch 0167, "4c" first message): the HFI transport of a6xx_hfi.c (queue write with
 * 0xfafafafa padding to 4 dwords, doorbell, MSGQ poll, response read with sequence match;
 * plus cache maintenance, since the CPU maps the hfi buffer cacheable and the GMU does not)
 * and the a8xx perf table HFI_H2F_MSG_TABLE (a8xx_hfi_send_perf_table): GPU levels of the
 * phone's speed bin 0xfc and the GMU frequency table from its DT, votes built as
 * a6xx_gmu_rpmh_arc_votes_init / a6xx_gmu_rpmh_dep_votes_init from cmd-db's level lists.
 *
 * Addresses: mainline gcc-kaanapali.c / gpucc-kaanapali.c and the phone's device tree
 * (SM8850, "canoe"); GMU registers as in Linux a6xx/a8xx (GPU base + dword offset * 4).
 * The gating order follows UniXNU's sheng GpuPowerBoard.h (BSD-2-Clause-Patent); no code
 * is copied from it.
 */
#if defined(PANDORA)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <arm/caches_internal.h>
#include <kern/kalloc.h>
#include "adreno/ArmSmmu4KStreams.h"
#include "adreno/ArmSmmu4KMmio.h"

extern kern_return_t thread_terminate(thread_t);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern int pandora_fw_read(uint64_t off, void *dst, uint64_t len);   /* bsd/dev/sweet_ufs.c */
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void pandora_scm_call(uint64_t fn, uint64_t arginfo, uint64_t x2, uint64_t x3,
    uint64_t *a0, uint64_t *a1);  /* bsd/dev/pandora_adsp.c */
void pandora_gpu_init(void);

#define SCM_FN(svc, cmd)  ((1ULL << 30) | (2ULL << 24) | (((uint64_t)(svc) & 0xff) << 8) | ((cmd) & 0xff))
#define SCM_ARGS(n)       ((uint64_t)(n) & 0xf)
#define SCM_SVC_GPU       0x28u
#define SCM_GPU_INIT_REGS 0x01u
#define GPU_ALWAYS_EN_REQ (1u << 0)
#define GPU_TSENSE_EN_REQ (1u << 3)

#define GCC_PA            0x00100000u
#define GCC_GPU_CFG_AHB   0x71004u     /* gcc_gpu_cfg_ahb_clk CBCR */
#define GCC_GEMNOC_GFX    0x71010u     /* gcc_gpu_gemnoc_gfx_clk CBCR */
#define GCC_GPLL0_VOTE    0x52000u     /* APCS PLL vote; bits 15/16: GPU votes */

#define GPUCC_PA          0x03d90000u
#define GPUCC_SIZE        0x9800u
#define GPUCC_PLL0        0x0000u
#define GPUCC_SMMU_VOTE   0x7000u      /* gpu_cc_..._smmu vote clock CBCR */
#define GPUCC_CX_GDSCR    0x9080u
#define GPUCC_CX_GDS_HW   0x9094u
#define GPUCC_AHB         0x90bcu
#define GPUCC_CX_GMU      0x90d4u
#define GPUCC_CXO         0x90e4u
#define GPUCC_HUB_CX_INT  0x90e8u
#define GPUCC_GMU_RCG     0x9318u
#define GPUCC_HUB_RCG     0x93f0u
#define GPUCC_SCAN_FIRST  0x9000u
#define GPUCC_SCAN_END    0x95ecu      /* regmap max_register 0x95e8 */

#define SMMU_PA           0x03da0000u  /* GR0 page */
#define SMMU_SCR0         0x000u
#define SMMU_ID0          0x020u
#define SMMU_ID1          0x024u
#define SMMU_ID2          0x028u
#define SMMU_SGFSR        0x048u
#define SMMU_SMR(n)       (0x800u + 4u * (n))
#define SMMU_S2CR(n)      (0xc00u + 4u * (n))

#define GPU_PA            0x03d00000u
#define GMU_PA            0x03d37000u  /* phone DT: gmu reg, 0x68000 */
#define GMU_SIZE          0x68000u
#define GMU_REG(dw)       (GPU_PA + 4u * (dw) - GMU_PA)

#define CMDDB_PA          0x81c60000u
#define CMDDB_SIZE        0x20000u

#define CBCR_CLK_OFF      (1u << 31)
#define CBCR_CLK_ENABLE   (1u << 0)
#define GDSCR_PWR_ON      (1u << 31)

static int gpu_chan;

static void
gpu_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&gpu_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static uint32_t
gpu_rd(vm_offset_t b, uint32_t o)
{
	uint32_t v = *(volatile uint32_t *)(b + o);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

/* Log what comes next and let the log thread write it before the read happens. */
static void
gpu_step(const char *what)
{
	printf("PANDORA GPU: next: %s\n", what);
	gpu_sleep(3500);
}

/* ---- cmd-db (same layout as pandora_adsp.c) ------------------------------------------- */

static vm_offset_t gpu_db;

static uint16_t
db16(uint32_t off)
{
	return (uint16_t)(gpu_rd(gpu_db, off & ~3u) >> (8u * (off & 3u)));
}

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
		for (unsigned j = 0; j < count && entries + 24 * (j + 1) <= CMDDB_SIZE; j++) {
			uint32_t e = entries + 24 * j;
			if (gpu_rd(gpu_db, e) == key[0] && gpu_rd(gpu_db, e + 4) == key[1]) {
				*addr = gpu_rd(gpu_db, e + 16);
				*auxlen = db16(e + 20);
				*aux = data + db16(e + 22);
				return 0;
			}
		}
	}
	return ENOENT;
}

static void
gpu_cmddb(void)
{
	static const char *const res[] = {
		"gfx.lvl", "mx.lvl", "cx.lvl", "gmxc.lvl", "mxc.lvl", "SH0", "MC0", "ACV", "CN0",
	};
	gpu_db = ml_io_map(CMDDB_PA, CMDDB_SIZE);
	if (!gpu_db) {
		printf("PANDORA GPU: cmd-db not mapped\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(res) / sizeof(res[0]); i++) {
		uint32_t addr, aux, auxlen;
		if (cmddb_find(res[i], &addr, &aux, &auxlen)) {
			printf("PANDORA GPU: cmd-db %s: not found\n", res[i]);
			continue;
		}
		char line[160];
		int n = snprintf(line, sizeof(line), "PANDORA GPU: cmd-db %s: addr %#x aux %u:", res[i],
		    addr, auxlen);
		for (uint32_t k = 0; k + 2 <= auxlen && k < 32 && n > 0 && (size_t)n < sizeof(line) - 6; k += 2) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %x", db16(aux + k));
		}
		printf("%s\n", line);
	}
}

/* ---- level 1 ---------------------------------------------------------------------------- */

static void
gpu_dump(vm_offset_t b, uint32_t first, uint32_t end, const char *name)
{
	for (uint32_t o = first; o < end; o += 32) {
		uint32_t v[8] = {0};
		bool any = false;
		for (unsigned k = 0; k < 8 && o + 4 * k < end; k++) {
			v[k] = gpu_rd(b, o + 4 * k);
			any |= v[k] != 0;
		}
		if (any) {
			printf("PANDORA GPU: %s %04x: %08x %08x %08x %08x %08x %08x %08x %08x\n", name, o,
			    v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
		}
	}
}

static void
gpu_wr(vm_offset_t b, uint32_t o, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(b + o) = v;
	__asm__ volatile ("dsb osh" ::: "memory");
}

/* Poll until (reg & mask) == want, at most us microseconds; returns the last value read. */
static uint32_t
gpu_poll(vm_offset_t b, uint32_t o, uint32_t mask, uint32_t want, unsigned us, bool *ok)
{
	uint32_t v = gpu_rd(b, o);
	for (unsigned t = 0; (v & mask) != want && t < us; t += 10) {
		delay(10);
		v = gpu_rd(b, o);
	}
	*ok = (v & mask) == want;
	return v;
}

static vm_offset_t gpu_cc;

/* GCC (always on), then GPUCC if gcc_gpu_cfg_ahb is enabled. False: GPUCC not readable. */
static bool
gpu_read_cc(bool full)
{
	vm_offset_t gcc71 = ml_io_map(GCC_PA + GCC_GPU_CFG_AHB - 0x4u, 0x1000);
	vm_offset_t gcc52 = ml_io_map(GCC_PA + GCC_GPLL0_VOTE, 0x1000);
	if (!gcc71 || !gcc52) {
		printf("PANDORA GPU: GCC not mapped\n");
		return false;
	}
	uint32_t cfg_ahb = gpu_rd(gcc71, 4), gemnoc = gpu_rd(gcc71, GCC_GEMNOC_GFX - GCC_GPU_CFG_AHB + 4);
	uint32_t gpll0 = gpu_rd(gcc52, 0);
	printf("PANDORA GPU: GCC gpu_cfg_ahb %08x gemnoc_gfx %08x gpll0_vote %08x (gpu bits %u%u)\n",
	    cfg_ahb, gemnoc, gpll0, (gpll0 >> 16) & 1, (gpll0 >> 15) & 1);
	if (!(cfg_ahb & CBCR_CLK_ENABLE)) {
		printf("PANDORA GPU: gpu_cfg_ahb not enabled: GPUCC not read\n");
		return false;
	}
	if (!gpu_cc) {
		gpu_step("GPUCC");
		gpu_cc = ml_io_map(GPUCC_PA, GPUCC_SIZE);
		if (!gpu_cc) {
			printf("PANDORA GPU: GPUCC not mapped\n");
			return false;
		}
	}
	vm_offset_t cc = gpu_cc;
	printf("PANDORA GPU: GPUCC cx_gdscr %08x hw %08x ahb %08x cx_gmu %08x cxo %08x hub_cx_int %08x "
	    "smmu_vote %08x\n", gpu_rd(cc, GPUCC_CX_GDSCR), gpu_rd(cc, GPUCC_CX_GDS_HW), gpu_rd(cc, GPUCC_AHB),
	    gpu_rd(cc, GPUCC_CX_GMU), gpu_rd(cc, GPUCC_CXO), gpu_rd(cc, GPUCC_HUB_CX_INT),
	    gpu_rd(cc, GPUCC_SMMU_VOTE));
	printf("PANDORA GPU: GPUCC gmu_rcg %08x %08x hub_rcg %08x %08x\n", gpu_rd(cc, GPUCC_GMU_RCG),
	    gpu_rd(cc, GPUCC_GMU_RCG + 4), gpu_rd(cc, GPUCC_HUB_RCG), gpu_rd(cc, GPUCC_HUB_RCG + 4));
	if (full) {
		gpu_dump(cc, GPUCC_PLL0, GPUCC_PLL0 + 0x40, "GPUCC pll0");
		gpu_dump(cc, GPUCC_SMMU_VOTE, GPUCC_SMMU_VOTE + 0x100, "GPUCC");
		gpu_dump(cc, GPUCC_SCAN_FIRST, GPUCC_SCAN_END, "GPUCC");
	}
	return true;
}

static bool
gpu_cx_on(void)
{
	return gpu_rd(gpu_cc, GPUCC_CX_GDSCR) & GDSCR_PWR_ON;
}

/* GPU SMMU, GR0 page, one stream-match entry at a time; only with CX on and its clock running. */
static void
gpu_read_smmu(void)
{
	uint32_t smmu_vote = gpu_rd(gpu_cc, GPUCC_SMMU_VOTE);
	bool cx_on = gpu_cx_on();
	if (!cx_on || (smmu_vote & CBCR_CLK_OFF)) {
		printf("PANDORA GPU: CX %s, SMMU vote clock %s: SMMU not read\n", cx_on ? "on" : "off",
		    (smmu_vote & CBCR_CLK_OFF) ? "off" : "on");
		return;
	}
	gpu_step("GPU SMMU GR0 ID registers");
	vm_offset_t smmu = ml_io_map(SMMU_PA, 0x1000);
	if (!smmu) {
		printf("PANDORA GPU: SMMU not mapped\n");
		return;
	}
	uint32_t id0 = gpu_rd(smmu, SMMU_ID0);
	printf("PANDORA GPU: SMMU sCR0 %08x ID0 %08x ID1 %08x ID2 %08x sGFSR %08x\n",
	    gpu_rd(smmu, SMMU_SCR0), id0, gpu_rd(smmu, SMMU_ID1), gpu_rd(smmu, SMMU_ID2),
	    gpu_rd(smmu, SMMU_SGFSR));
	unsigned nsmr = MIN(id0 & 0xffu, 128u), valid = 0;
	for (unsigned i = 0; i < nsmr; i++) {
		if (i % 16 == 0) {
			char what[48];
			snprintf(what, sizeof(what), "SMMU SMR/S2CR %u..%u of %u", i, MIN(i + 15, nsmr - 1), nsmr);
			gpu_step(what);
		}
		uint32_t smr = gpu_rd(smmu, SMMU_SMR(i));
		uint32_t s2cr = gpu_rd(smmu, SMMU_S2CR(i));
		if (smr & (1u << 31)) {
			valid++;
			printf("PANDORA GPU: SMMU SMR[%u] %08x (id %#x mask %#x) S2CR %08x\n", i, smr,
			    smr & 0xffffu, (smr >> 16) & 0x7fffu, s2cr);
		}
	}
	printf("PANDORA GPU: SMMU %u of %u stream-match entries valid\n", valid, nsmr);
}

/* GMU state; only with CX on and gpu_cc_ahb and cx_gmu running. */
static void
gpu_read_gmu(void)
{
	uint32_t ahb = gpu_rd(gpu_cc, GPUCC_AHB), cx_gmu = gpu_rd(gpu_cc, GPUCC_CX_GMU);
	bool cx_on = gpu_cx_on();
	if (!cx_on || (ahb & CBCR_CLK_OFF) || (cx_gmu & CBCR_CLK_OFF)) {
		printf("PANDORA GPU: CX %s, gpu_cc_ahb %s, cx_gmu %s: GMU not read\n", cx_on ? "on" : "off",
		    (ahb & CBCR_CLK_OFF) ? "off" : "on", (cx_gmu & CBCR_CLK_OFF) ? "off" : "on");
		return;
	}
	gpu_step("GMU state registers");
	vm_offset_t gmu = ml_io_map(GMU_PA, GMU_SIZE);
	if (!gmu) {
		printf("PANDORA GPU: GMU window not mapped\n");
		return;
	}
	printf("PANDORA GPU: GMU cm3_sysreset %08x fw_init_result %08x hfi_ctrl_status %08x "
	    "gfx_pwr_clk_status %08x rpmh_power_state %08x ao_host_irq %08x\n",
	    gpu_rd(gmu, GMU_REG(0x1f800)), gpu_rd(gmu, GMU_REG(0x1f81c)), gpu_rd(gmu, GMU_REG(0x1f980)),
	    gpu_rd(gmu, GMU_REG(0x1f7e8)), gpu_rd(gmu, GMU_REG(0x1f7e9)), gpu_rd(gmu, GMU_REG(0x23b05)));
	printf("PANDORA GPU: RSCC status %08x CX_MISC tcm_ret %08x\n",
	    gpu_rd(gmu, 0x03d50000u + 0x101u * 4 - GMU_PA), gpu_rd(gmu, 0x03d9e000u + 0x39u * 4 - GMU_PA));
}

/* Enable a branch clock (CBCR bit 0) and wait for CLK_OFF to clear. */
static bool
gpu_branch_on(uint32_t cbcr, const char *name)
{
	bool ok;
	uint32_t v = gpu_rd(gpu_cc, cbcr);
	if (!(v & CBCR_CLK_OFF)) {
		printf("PANDORA GPU: %s already running (%08x)\n", name, v);
		return true;
	}
	gpu_wr(gpu_cc, cbcr, v | CBCR_CLK_ENABLE);
	v = gpu_poll(gpu_cc, cbcr, CBCR_CLK_OFF, 0, 500, &ok);
	printf("PANDORA GPU: %s on: %08x %s\n", name, v, ok ? "running" : "STILL OFF");
	return ok;
}

/* Level 2: CX GDSC on (gdsc.c: clear SW_COLLAPSE, poll PWR_ON in gds_hw_ctrl), clocks on. */
static bool
gpu_level2(void)
{
	bool ok;
	uint32_t g = gpu_rd(gpu_cc, GPUCC_CX_GDSCR);
	if (g & GDSCR_PWR_ON) {
		printf("PANDORA GPU: CX GDSC already on (%08x)\n", g);
	} else {
		gpu_step("CX GDSC on (clear SW_COLLAPSE)");
		gpu_wr(gpu_cc, GPUCC_CX_GDSCR, g & ~1u);
		uint32_t hw = gpu_poll(gpu_cc, GPUCC_CX_GDS_HW, GDSCR_PWR_ON, GDSCR_PWR_ON, 1000, &ok);
		printf("PANDORA GPU: CX GDSC: gdscr %08x hw %08x %s\n", gpu_rd(gpu_cc, GPUCC_CX_GDSCR), hw,
		    ok ? "on" : "DID NOT TURN ON");
		if (!ok) {
			return false;
		}
	}
	gpu_step("gpu_cc_ahb and SMMU vote clock on");
	bool a = gpu_branch_on(GPUCC_AHB, "gpu_cc_ahb");
	bool s = gpu_branch_on(GPUCC_SMMU_VOTE, "SMMU vote clock");
	return a && s;
}

/* Level 3: GPLL0 votes for the GPU, then SCM GPU_INIT_REGS. */
static bool
gpu_level3(void)
{
	vm_offset_t gcc52 = ml_io_map(GCC_PA + GCC_GPLL0_VOTE, 0x1000);
	if (!gcc52) {
		printf("PANDORA GPU: GCC not mapped\n");
		return false;
	}
	gpu_step("GPLL0 votes for the GPU (GCC 0x52000 bits 15/16)");
	uint32_t v = gpu_rd(gcc52, 0);
	gpu_wr(gcc52, 0, v | (1u << 15) | (1u << 16));
	delay(50);
	printf("PANDORA GPU: gpll0_vote %08x -> %08x\n", v, gpu_rd(gcc52, 0));

	uint64_t a0, a1;
	uint64_t id = (2ULL << 24) | (SCM_SVC_GPU << 8) | SCM_GPU_INIT_REGS;
	pandora_scm_call(SCM_FN(6, 1), SCM_ARGS(1), id, 0, &a0, &a1);
	printf("PANDORA GPU: SCM is_call_avail(gpu, init_regs) -> a0 %#llx a1 %#llx\n", a0, a1);
	gpu_step("SCM GPU_INIT_REGS (ALWAYS_EN | TSENSE_EN)");
	pandora_scm_call(SCM_FN(SCM_SVC_GPU, SCM_GPU_INIT_REGS), SCM_ARGS(1),
	    GPU_ALWAYS_EN_REQ | GPU_TSENSE_EN_REQ, 0, &a0, &a1);
	printf("PANDORA GPU: SCM GPU_INIT_REGS(%#x) -> a0 %#llx a1 %#llx\n",
	    GPU_ALWAYS_EN_REQ | GPU_TSENSE_EN_REQ, a0, a1);
	return a0 == 0;
}

/* ---- level 4a: the GMU's SMMU context ------------------------------------------------- */

#define SMMU_SIZE         0x40000u     /* DT kgsl-smmu reg length: 32 global + 32 context pages */
#define GMU_SID           5u           /* DT gmu iommus = <&kgsl_smmu 0x5 0x0> */
#define GMU_ACTLR         0x32bu       /* DT qcom,actlr for the GPU SMMU's streams */
#define GMU_BANK          1u
#define GMU_ASID          1u
#define GMU_SLOT          2u
#define GMU_TABLE_PAGES   64u

struct gmu_buf {
	const char *name;
	uint64_t iova, bytes;
	unsigned perm;
	vm_offset_t va;
	uint64_t pa;
};

/* Linux a6xx_gmu_init / a6xx_gmu_memory_alloc for a8xx, sorted by GMU address. */
static struct gmu_buf gmu_bufs[] = {
	{ "icache", 0x00004000u, 0xffc000u, ArmSmmu4KRead | ArmSmmu4KWrite | ArmSmmu4KExecute | ArmSmmu4KPrivileged, 0, 0 },
	{ "dummy", 0x60000000u, 0x4000u, ArmSmmu4KRead | ArmSmmu4KWrite, 0, 0 },
	{ "log", 0x60004000u, 0x4000u, ArmSmmu4KRead | ArmSmmu4KWrite, 0, 0 },
	{ "hfi", 0x60008000u, 0x4000u, ArmSmmu4KRead | ArmSmmu4KWrite, 0, 0 },
	{ "debug", 0x60400000u, 0x7000u, ArmSmmu4KRead | ArmSmmu4KWrite, 0, 0 },
};
#define GMU_NBUFS (sizeof(gmu_bufs) / sizeof(gmu_bufs[0]))

static ArmSmmu4KMmio gmu_mmio;
static ArmSmmu4KIo gmu_io;
static ArmSmmu4KConfig gmu_tcfg;
static ArmSmmu4KResult gmu_tables;
static ArmSmmu4KContextConfig gmu_ctx;
static ArmSmmu4KProgramResult gmu_prog;
static ArmSmmu4KStreamState gmu_streams;
static ArmSmmu4KStreamResult gmu_sres;

/* Physically contiguous, zeroed, wired for good; returns the kernel address and fills *pa. */
static vm_offset_t
gpu_contig(uint64_t bytes, uint64_t *pa)
{
	vm_offset_t va = 0;
	if (kmem_alloc_contig(kernel_map, &va, (vm_size_t)bytes, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return 0;
	}
	*pa = ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)va));
	/* The SMMU walks the tables and the GMU reads the buffers non-cacheably. */
	clean_dcache64((addr64_t)*pa, (unsigned)bytes, TRUE);
	return va;
}

static bool
gpu_level4a(void)
{
	/* Memory. */
	for (unsigned i = 0; i < GMU_NBUFS; i++) {
		struct gmu_buf *b = &gmu_bufs[i];
		b->va = gpu_contig(b->bytes, &b->pa);
		if (!b->va) {
			printf("PANDORA GPU: GMU %s: no %#llx contiguous bytes\n", b->name, b->bytes);
			return false;
		}
		printf("PANDORA GPU: GMU %s: iova %#llx bytes %#llx pa %#llx\n", b->name, b->iova, b->bytes, b->pa);
	}
	uint64_t tpa;
	vm_offset_t tva = gpu_contig(GMU_TABLE_PAGES * 0x1000ull, &tpa);
	if (!tva) {
		printf("PANDORA GPU: GMU tables: no memory\n");
		return false;
	}

	/* SMMU window and what the context encoding needs from it. */
	vm_offset_t smmu = ml_io_map(SMMU_PA, SMMU_SIZE);
	if (!smmu) {
		printf("PANDORA GPU: SMMU window not mapped\n");
		return false;
	}
	gmu_mmio.mapping = (volatile unsigned char *)smmu;
	gmu_mmio.bytes = SMMU_SIZE;
	if (ArmSmmu4KBindMmio(&gmu_mmio, &gmu_io) != ArmSmmu4KSuccess) {
		return false;
	}
	uint32_t id0 = gpu_rd(smmu, SMMU_ID0), id1 = gpu_rd(smmu, SMMU_ID1), id2 = gpu_rd(smmu, SMMU_ID2);
	static const uint8_t oas_bits[16] = {32, 36, 40, 42, 44, 48};
	unsigned oas = ((id2 >> 4) & 15) < 6 ? oas_bits[(id2 >> 4) & 15] : 0;
	if (oas > 40) {
		oas = 40;
	}

	/* Tables. */
	ArmSmmu4KRange ranges[GMU_NBUFS];
	uint64_t leaves = 0;
	for (unsigned i = 0; i < GMU_NBUFS; i++) {
		ranges[i] = (ArmSmmu4KRange){ gmu_bufs[i].iova, gmu_bufs[i].pa, gmu_bufs[i].bytes,
			                      gmu_bufs[i].perm, 0, 2 };
		leaves += gmu_bufs[i].bytes / 0x1000;
		if ((gmu_bufs[i].pa >> oas) != 0) {
			printf("PANDORA GPU: GMU %s above the %u-bit output size\n", gmu_bufs[i].name, oas);
			return false;
		}
	}
	gmu_tcfg = (ArmSmmu4KConfig){ 32, oas, tpa, (size_t)leaves };
	ArmSmmu4KStatus st = ArmSmmu4KBuild(&gmu_tcfg, ranges, GMU_NBUFS, (uint64_t *)tva,
	    GMU_TABLE_PAGES, &gmu_tables);
	printf("PANDORA GPU: GMU tables: status %d root %#llx pages %zu leaves %zu start level %u mair %#llx "
	    "(oas %u, id0 %08x id1 %08x id2 %08x)\n", st, gmu_tables.root_physical, gmu_tables.table_pages,
	    gmu_tables.leaf_pages, gmu_tables.start_level, gmu_tables.mair, oas, id0, id1, id2);
	if (st != ArmSmmu4KSuccess) {
		return false;
	}
	clean_dcache64((addr64_t)tpa, (unsigned)(gmu_tables.table_pages * 0x1000), TRUE);

	/* Context bank: must be disabled; ACTLR is the platform's part (the kit does not write it). */
	uint64_t page = (id1 & (1u << 31)) ? 0x10000 : 0x1000;
	uint64_t numpage = 1ull << (((id1 >> 28) & 7) + 1);
	uint64_t cb = numpage * page + GMU_BANK * page;
	if (cb + 0x1000 > SMMU_SIZE) {
		printf("PANDORA GPU: context bank %u at %#llx outside the window\n", GMU_BANK, cb);
		return false;
	}
	uint32_t sctlr = gpu_rd(smmu, (uint32_t)cb);
	printf("PANDORA GPU: SMMU bank %u at %#llx: sctlr %08x actlr %08x cbar %08x\n", GMU_BANK, cb, sctlr,
	    gpu_rd(smmu, (uint32_t)cb + 4), gpu_rd(smmu, (uint32_t)(page + 4 * GMU_BANK)));
	if (sctlr & 1) {
		printf("PANDORA GPU: bank %u already enabled: stop\n", GMU_BANK);
		return false;
	}
	gpu_step("SMMU: ACTLR, then program and enable the GMU context");
	gpu_wr(smmu, (uint32_t)cb + 4, GMU_ACTLR);
	gmu_ctx = (ArmSmmu4KContextConfig){ &gmu_tcfg, NULL, &gmu_tables, NULL, id0, id2, GMU_ASID, 0, 0, 0 };
	st = ArmSmmu4KProgramDisabledContext(&gmu_io, GMU_BANK, &gmu_ctx, 1000, &gmu_prog);
	printf("PANDORA GPU: SMMU context: status %d phase %u writes %u enabled %u fsr %08x actlr %08x sctlr %08x "
	    "tcr %08x ttbr0 %#llx mair0 %08x\n", st, gmu_prog.phase, gmu_prog.writes, gmu_prog.enabled,
	    gmu_prog.fault_status, gpu_rd(smmu, (uint32_t)cb + 4), gmu_prog.readback.sctlr, gmu_prog.readback.tcr,
	    gmu_prog.readback.ttbr0, gmu_prog.readback.mair0);
	if (st != ArmSmmu4KSuccess) {
		return false;
	}

	/* Stream: the free slot's S2CR must fault before the kit attaches it. */
	uint32_t smr = gpu_rd(smmu, SMMU_SMR(GMU_SLOT)), s2cr = gpu_rd(smmu, SMMU_S2CR(GMU_SLOT));
	printf("PANDORA GPU: SMMU slot %u before: smr %08x s2cr %08x\n", GMU_SLOT, smr, s2cr);
	if (smr & (1u << 31)) {
		printf("PANDORA GPU: slot %u in use: stop\n", GMU_SLOT);
		return false;
	}
	if (s2cr != (2u << 16)) {
		gpu_wr(smmu, SMMU_S2CR(GMU_SLOT), 2u << 16);
		printf("PANDORA GPU: slot %u S2CR set to fault: %08x\n", GMU_SLOT, gpu_rd(smmu, SMMU_S2CR(GMU_SLOT)));
	}
	gpu_step("SMMU: route stream 5 to the GMU context");
	ArmSmmu4KStream route = { GMU_SLOT, (uint16_t)GMU_SID };
	st = ArmSmmu4KAttachStreams(&gmu_io, &gmu_streams, &route, 1, GMU_BANK, &gmu_ctx, &gmu_sres);
	printf("PANDORA GPU: SMMU stream: status %d phase %u writes %u verified %u global fault %08x; "
	    "slot %u smr %08x s2cr %08x\n", st, gmu_sres.phase, gmu_sres.writes, gmu_sres.streams_verified,
	    gmu_sres.global_fault, GMU_SLOT, gpu_rd(smmu, SMMU_SMR(GMU_SLOT)), gpu_rd(smmu, SMMU_S2CR(GMU_SLOT)));
	return st == ArmSmmu4KSuccess;
}

/* ---- level 5 (4b): GMU cold start --------------------------------------------------- */

/* GMU registers, dword offsets from the GPU base (a6xx_gmu.xml, A8XX variants where they differ). */
#define GMU_CM3_ITCM_START        0x1b400u
#define GMU_CM3_DTCM_START        0x1c400u
#define GMU_CORE_FW_VERSION       0x1d3f8u     /* = DTCM_START + 0xff8 */
#define GMU_ICACHE_CONFIG         0x1f400u
#define GMU_DCACHE_CONFIG         0x1f401u
#define GMU_SYS_BUS_CONFIG        0x1f40fu
#define GMU_MRC_GBIF_QOS_CTRL     0x1f50bu
#define GMU_CX_FAL_INTF           0x1f7ecu     /* A8XX */
#define GMU_CX_FALNEXT_INTF       0x1f7edu     /* A8XX */
#define GMU_CM3_SYSRESET          0x1f800u
#define GMU_CM3_BOOT_CONFIG       0x1f801u
#define GMU_CM3_FW_INIT_RESULT    0x1f81cu
#define GMU_CM3_CFG               0x1f82du
#define GMU_HFI_CTRL_STATUS       0x1f980u
#define GMU_HFI_QTBL_INFO         0x1f984u
#define GMU_HFI_QTBL_ADDR         0x1f985u
#define GMU_HFI_CTRL_INIT         0x1f986u
#define GMU_GENERAL_8             0x1f9c8u     /* A8XX */
#define GMU_GENERAL_9             0x1f9c9u     /* A8XX */
#define GMU_GENERAL_10            0x1f9cau     /* A8XX */
#define GMU_AO_HOST_INTR_STATUS   0x23b05u
#define GMU_AHB_FENCE_RANGE_0     0x23b11u
#define GBIF_CX_CONFIG            0x3c00u
#define GBIF_QSB_SIDE0            0x3c03u
#define GMU_DTCM_BASE             0x10004000u  /* a6xx_gmu_fw_load: a650 family, a7xx and a8xx */
#define GMU_CHIPID                0x08020100u  /* a6xx_catalog.c, chip 0x44050a01 */

static vm_offset_t gpu_gmu;

static uint32_t
gmu_rd(uint32_t dw)
{
	return gpu_rd(gpu_gmu, GMU_REG(dw));
}

static void
gmu_wr(uint32_t dw, uint32_t v)
{
	gpu_wr(gpu_gmu, GMU_REG(dw), v);
}

struct pack_ent {
	char name[48];
	uint64_t off, size;
};

/* A file from the pinned firmware pack (tools/mkfwpack.py) into a new kalloc_data buffer. */
static uint8_t *
gpu_fw_file(const char *name, uint64_t *size)
{
	static uint8_t dir[4096];
	if (pandora_fw_read(0, dir, sizeof(dir)) || memcmp(dir, "PANDFW01", 8)) {
		printf("PANDORA GPU: no firmware pack\n");
		return NULL;
	}
	uint32_t n;
	memcpy(&n, dir + 8, 4);
	for (uint32_t i = 0; i < n && i < 63; i++) {
		struct pack_ent e;
		memcpy(&e, dir + 16 + 64 * i, sizeof(e));
		e.name[47] = 0;
		if (strcmp(e.name, name) || e.size == 0 || e.size > (1u << 20)) {
			continue;
		}
		uint8_t *buf = kalloc_data((vm_size_t)e.size, Z_WAITOK);
		if (!buf || pandora_fw_read(e.off, buf, e.size)) {
			printf("PANDORA GPU: %s: read failed\n", name);
			return NULL;
		}
		*size = e.size;
		return buf;
	}
	printf("PANDORA GPU: %s not in the firmware pack\n", name);
	return NULL;
}

static struct gmu_buf *
gmu_buf_for(uint32_t addr, uint32_t size)
{
	for (unsigned i = 0; i < GMU_NBUFS; i++) {
		if (addr >= gmu_bufs[i].iova && size <= gmu_bufs[i].bytes &&
		    addr - gmu_bufs[i].iova <= gmu_bufs[i].bytes - size) {
			return &gmu_bufs[i];
		}
	}
	return NULL;
}

/* a6xx_gmu_fw_load: route each block by address; registers get whole words, read back. */
static bool
gmu_fw_load(const uint8_t *fw, uint64_t len)
{
	unsigned itcm = 0, dtcm = 0, mem = 0, bad = 0;
	for (uint64_t off = 0; off + 16 <= len;) {
		uint32_t h[4];
		memcpy(h, fw + off, 16);
		uint32_t addr = h[0], size = h[1];
		const uint8_t *data = fw + off + 16;
		if (size > len - off - 16) {
			printf("PANDORA GPU: fw block at %#llx: size %#x past the end\n", off, size);
			return false;
		}
		off += 16 + (uint64_t)size;
		if (size == 0) {
			continue;
		}
		bool tcm = addr < 0x4000 || (addr >= GMU_DTCM_BASE && addr - GMU_DTCM_BASE < 0x4000);
		if (tcm) {
			if (size < 4) {
				/* Linux writes these bytes too; the full ITCM block after them covers them. */
				printf("PANDORA GPU: fw block addr %#x size %u type %#x skipped\n", addr, size, h[2]);
				continue;
			}
			if ((size & 3) || (addr & 3)) {
				printf("PANDORA GPU: fw block addr %#x size %#x not word sized\n", addr, size);
				return false;
			}
			uint32_t reg = addr < 0x4000 ? GMU_CM3_ITCM_START + (addr >> 2) :
			    GMU_CM3_DTCM_START + ((addr - GMU_DTCM_BASE) >> 2);
			for (uint32_t k = 0; k < size / 4; k++) {
				uint32_t w;
				memcpy(&w, data + 4 * k, 4);
				gmu_wr(reg + k, w);
			}
			for (uint32_t k = 0; k < size / 4; k++) {
				uint32_t w;
				memcpy(&w, data + 4 * k, 4);
				bad += gmu_rd(reg + k) != w;
			}
			if (addr < 0x4000) {
				itcm += size;
			} else {
				dtcm += size;
			}
			continue;
		}
		struct gmu_buf *b = gmu_buf_for(addr, size);
		if (!b) {
			printf("PANDORA GPU: fw block addr %#x size %#x matches no buffer\n", addr, size);
			return false;
		}
		memcpy((void *)(b->va + (addr - b->iova)), data, size);
		clean_dcache64((addr64_t)(b->pa + (addr - b->iova)), size, TRUE);
		mem += size;
	}
	printf("PANDORA GPU: GMU firmware loaded: ITCM %#x DTCM %#x memory %#x bytes, %u register words differ, "
	    "core fw version %08x\n", itcm, dtcm, mem, bad, gmu_rd(GMU_CORE_FW_VERSION));
	return bad == 0;
}

/* a6xx_hfi_init: queue table header and three queue headers at the start of the hfi buffer. */
static void
gmu_hfi_init(void)
{
	struct gmu_buf *h = &gmu_bufs[3];
	uint32_t *t = (uint32_t *)h->va;
	bzero(t, (size_t)h->bytes);
	t[0] = 0;               /* version */
	t[1] = 24 + 3 * 48;     /* size: the header and the queue headers, in bytes as Linux writes it */
	t[2] = 24 >> 2;         /* qhdr0_offset */
	t[3] = 48 >> 2;         /* qhdr_size */
	t[4] = 3;               /* num_queues */
	t[5] = 3;               /* active_queues */
	for (uint32_t q = 0; q < 3; q++) {
		uint32_t *qh = t + 6 + 12 * q;
		qh[0] = 1;                                     /* status */
		qh[1] = (uint32_t)h->iova + 0x1000 * (q + 1);  /* iova */
		qh[2] = (10u << 8) | q;                        /* type: CMD 0, RESP 1, DBG 2 (non-legacy) */
		qh[3] = 0x1000 >> 2;                           /* size in dwords */
		qh[6] = 1;                                     /* rx_watermark */
		qh[7] = 1;                                     /* tx_watermark */
		qh[8] = 1;                                     /* rx_request */
	}
	clean_dcache64((addr64_t)h->pa, (unsigned)h->bytes, TRUE);
}

static bool
gpu_level5(void)
{
	bool ok;
	gpu_gmu = ml_io_map(GMU_PA, GMU_SIZE);
	vm_offset_t gbif = ml_io_map(GPU_PA + GBIF_CX_CONFIG * 4, 0x1000);
	if (!gpu_gmu || !gbif) {
		printf("PANDORA GPU: GMU or GBIF window not mapped\n");
		return false;
	}
	uint32_t rst = gmu_rd(GMU_CM3_SYSRESET), hfi = gmu_rd(GMU_HFI_CTRL_STATUS);
	uint32_t tcm = gpu_rd(gpu_gmu, 0x03d9e000u + 0x39u * 4 - GMU_PA);
	uint32_t rscc = gpu_rd(gpu_gmu, 0x03d50000u + 0x101u * 4 - GMU_PA);
	if (rst != 1 || (hfi & 1) || tcm || rscc) {
		printf("PANDORA GPU: GMU not idle in reset (sysreset %u hfi %08x tcm_ret %u rscc %u): stop\n",
		    rst, hfi, tcm, rscc);
		return false;
	}
	uint64_t fwlen = 0;
	uint8_t *fw = gpu_fw_file("gen80200_gmu.bin", &fwlen);
	if (!fw) {
		return false;
	}

	/* a6xx_gmu_fw_start, a8xx path. */
	gpu_step("GMU: FAL veto, firmware, HFI table, configuration");
	gmu_wr(GMU_CX_FALNEXT_INTF, 1);
	gmu_wr(GMU_CX_FAL_INTF, 1);
	bool loaded = gmu_fw_load(fw, fwlen);
	kfree_data(fw, (vm_size_t)fwlen);
	if (!loaded) {
		return false;
	}
	gmu_hfi_init();
	gmu_wr(GMU_CM3_FW_INIT_RESULT, 0);
	gmu_wr(GMU_CM3_BOOT_CONFIG, 0x02);
	gmu_wr(GMU_HFI_QTBL_ADDR, (uint32_t)gmu_bufs[3].iova);
	gmu_wr(GMU_HFI_QTBL_INFO, 1);
	gmu_wr(GMU_AHB_FENCE_RANGE_0, (1u << 31) | (0x32u << 18) | 0x8c0u);
	gmu_wr(GMU_CM3_CFG, 0x4052);
	gmu_wr(GMU_GENERAL_10, GMU_CHIPID);
	gmu_wr(GMU_GENERAL_8, ((uint32_t)gmu_bufs[2].iova & 0xfffff000u) |
	    ((uint32_t)(gmu_bufs[2].bytes / 0x1000 - 1) & 0xffu));
	for (uint32_t i = 0; i < 4; i++) {
		gpu_wr(gbif, 4 * (GBIF_QSB_SIDE0 + i - GBIF_CX_CONFIG), 0x00071e20);   /* a840_gbif */
	}
	gpu_wr(gbif, 0, 0x20023000);    /* a840_gbif GBIF_CX_CONFIG, written again for a8xx */
	gpu_wr(gbif, 0, 0x20023000);
	gmu_wr(GMU_MRC_GBIF_QOS_CTRL, 0x33);
	gmu_wr(GMU_SYS_BUS_CONFIG, 1);  /* a6xx_gmu_power_config; a8xx returns after these three */
	gmu_wr(GMU_ICACHE_CONFIG, 1);
	gmu_wr(GMU_DCACHE_CONFIG, 1);
	printf("PANDORA GPU: GMU config: boot %x qtbl %08x/%x fence %08x cm3_cfg %08x gen10 %08x gen8 %08x "
	    "gbif cx %08x qsb0 %08x qos %x bus/i/d %x%x%x\n", gmu_rd(GMU_CM3_BOOT_CONFIG),
	    gmu_rd(GMU_HFI_QTBL_ADDR), gmu_rd(GMU_HFI_QTBL_INFO), gmu_rd(GMU_AHB_FENCE_RANGE_0),
	    gmu_rd(GMU_CM3_CFG), gmu_rd(GMU_GENERAL_10), gmu_rd(GMU_GENERAL_8), gpu_rd(gbif, 0),
	    gpu_rd(gbif, 4 * (GBIF_QSB_SIDE0 - GBIF_CX_CONFIG)), gmu_rd(GMU_MRC_GBIF_QOS_CTRL),
	    gmu_rd(GMU_SYS_BUS_CONFIG), gmu_rd(GMU_ICACHE_CONFIG), gmu_rd(GMU_DCACHE_CONFIG));

	/* a6xx_gmu_start. */
	uint32_t ver = gmu_rd(GMU_CORE_FW_VERSION);
	uint32_t mask = ver <= 0x20010004 ? 0xffffffffu : 0x1ffu, want = ver <= 0x20010004 ? 0xbabefaceu : 0x100u;
	gmu_wr(GMU_CM3_SYSRESET, 1);
	gmu_wr(GMU_GENERAL_9, 0);
	gpu_step("GMU: release CM3 reset");
	gmu_wr(GMU_CM3_SYSRESET, 0);
	uint32_t r = gpu_poll(gpu_gmu, GMU_REG(GMU_CM3_FW_INIT_RESULT), mask, want, 100000, &ok);
	printf("PANDORA GPU: GMU fw init result %08x (want %#x under mask %#x) %s; ao_host_irq %08x\n", r, want,
	    mask, ok ? "BOOTED" : "TIMED OUT", gmu_rd(GMU_AO_HOST_INTR_STATUS));
	if (!ok) {
		gmu_wr(GMU_CM3_SYSRESET, 1);
		return false;
	}

	/* a6xx_gmu_hfi_start. */
	gmu_wr(GMU_HFI_CTRL_INIT, 1);
	r = gpu_poll(gpu_gmu, GMU_REG(GMU_HFI_CTRL_STATUS), 1, 1, 10000, &ok);
	printf("PANDORA GPU: GMU HFI ctrl status %08x %s\n", r, ok ? "READY" : "NOT READY");
	const uint32_t *log = (const uint32_t *)gmu_bufs[2].va;
	flush_dcache64((addr64_t)gmu_bufs[2].pa, 64, TRUE);
	printf("PANDORA GPU: GMU log %08x %08x %08x %08x %08x %08x %08x %08x\n", log[0], log[1], log[2], log[3],
	    log[4], log[5], log[6], log[7]);
	return ok;
}

/* ---- level 6 (4c): HFI transport and the perf table --------------------------------- */

#define GMU_GMU2HOST_INTR_CLR     0x1f991u
#define GMU_GMU2HOST_INTR_INFO    0x1f992u
#define GMU_HOST2GMU_INTR_SET     0x1f994u
#define HFI_QUEUE_DWORDS          1024u
#define HFI_F2H_MSG_ERROR         100u
#define HFI_H2F_MSG_TABLE         15u
#define HFI_TABLE_GPU_PERF        1u

/* Phone DT qcom,gpu-pwrlevels-1 (speed-bin 0xfc; Android lists exactly these 18 frequencies),
 * ascending, with qcom,level; and qcom,gmu@3d37000 qcom,gmu-freq-table. */
static const struct { uint32_t hz, level; } gpu_opps[] = {
	{ 160000000, 0x32 }, { 191000000, 0x33 }, { 222000000, 0x34 }, { 282000000, 0x36 },
	{ 342000000, 0x38 }, { 382000000, 0x3c }, { 422000000, 0x40 }, { 461000000, 0x4c },
	{ 500000000, 0x50 }, { 578000000, 0x80 }, { 646000000, 0x90 }, { 726000000, 0xc0 },
	{ 826000000, 0xe0 }, { 902000000, 0x100 }, { 1000000000, 0x180 }, { 1050000000, 0x1a0 },
	{ 1100000000, 0x1b0 }, { 1200000000, 0x1c0 },
}, gmu_opps[] = {
	{ 475000000, 0x38 }, { 575000000, 0x40 }, { 700000000, 0x80 }, { 750000000, 0x100 },
};
#define NGPU (1 + sizeof(gpu_opps) / sizeof(gpu_opps[0]))      /* with the "off" level */
#define NGMU (1 + sizeof(gmu_opps) / sizeof(gmu_opps[0]))

static uint32_t hfi_seq;

/* cmd-db aux data of a .lvl resource: u16 levels. */
static unsigned
gpu_lvl(const char *res, uint16_t *out, unsigned max)
{
	uint32_t addr, aux, len;
	if (!gpu_db || cmddb_find(res, &addr, &aux, &len)) {
		return 0;
	}
	unsigned n = MIN(len / 2, max);
	for (unsigned k = 0; k < n; k++) {
		out[k] = db16(aux + 2 * k);
	}
	return n;
}

/* a6xx_gmu_rpmh_arc_votes_init for one level; false if the primary list has no such level. */
static bool
arc_vote(uint32_t level, const uint16_t *pri, unsigned np, const uint16_t *sec, unsigned ns, uint32_t *vote)
{
	unsigned p = 0, s = 0, j;
	for (j = 0; j < np; j++) {
		if (pri[j] >= level) {
			p = j;
			break;
		}
	}
	if (j == np) {
		return false;
	}
	for (j = 0; j < ns; j++) {
		if (sec[j] >= level) {
			s = j;
			break;
		} else if (sec[j]) {
			s = j;
		}
	}
	*vote = ((uint32_t)(pri[p] & 0xffff) << 16) | (s << 8) | p;
	return true;
}

static uint32_t *
hfi_q(unsigned q)
{
	return (uint32_t *)gmu_bufs[3].va + 6 + 12 * q;
}

static void
hfi_clean(const void *va, size_t len)
{
	addr64_t pa = gmu_bufs[3].pa + ((vm_offset_t)va - gmu_bufs[3].va);
	clean_dcache64(pa, (unsigned)len, TRUE);
}

static void
hfi_inval(const void *va, size_t len)
{
	addr64_t pa = gmu_bufs[3].pa + ((vm_offset_t)va - gmu_bufs[3].va);
	flush_dcache64(pa, (unsigned)len, TRUE);
}

/* a6xx_hfi_send_msg: msg[0] is filled with the header; true when the GMU acked without error. */
static bool
hfi_send(uint32_t id, uint32_t *msg, uint32_t dwords)
{
	uint32_t *cq = hfi_q(0), *rq = hfi_q(1);
	uint32_t *cdata = (uint32_t *)(gmu_bufs[3].va + 0x1000), *rdata = (uint32_t *)(gmu_bufs[3].va + 0x2000);
	uint32_t seq = (++hfi_seq) % 0xfff;
	msg[0] = (seq << 20) | (dwords << 8) | id;

	/* a6xx_hfi_queue_write */
	hfi_inval(cq, 48);
	uint32_t w = cq[11], r = cq[10];
	uint32_t space = (r - w - 1) & (HFI_QUEUE_DWORDS - 1);
	if (space < dwords) {
		printf("PANDORA GPU: HFI command queue full (r %u w %u)\n", r, w);
		return false;
	}
	for (uint32_t i = 0; i < dwords; i++) {
		cdata[w] = msg[i];
		w = (w + 1) % HFI_QUEUE_DWORDS;
	}
	for (; w % 4; w = (w + 1) % HFI_QUEUE_DWORDS) {
		cdata[w] = 0xfafafafa;
	}
	hfi_clean(cdata, 0x1000);
	__asm__ volatile ("dsb sy" ::: "memory");
	cq[11] = w;
	hfi_clean(cq, 48);
	__asm__ volatile ("dsb sy" ::: "memory");
	gmu_wr(GMU_HOST2GMU_INTR_SET, 1);

	/* a6xx_hfi_wait_for_ack */
	for (unsigned tries = 0; tries < 8; tries++) {
		bool ok;
		uint32_t info = gpu_poll(gpu_gmu, GMU_REG(GMU_GMU2HOST_INTR_INFO), 1, 1, 1000000, &ok);
		if (!ok) {
			printf("PANDORA GPU: HFI id %u seq %u: no response (intr info %08x, cmd r %u w %u)\n", id, seq,
			    info, (hfi_inval(cq, 48), cq[10]), cq[11]);
			return false;
		}
		gmu_wr(GMU_GMU2HOST_INTR_CLR, 1);
		for (;;) {
			hfi_inval(rq, 48);
			hfi_inval(rdata, 0x1000);
			uint32_t ri = rq[10];
			if (ri == rq[11]) {
				rq[8] = 1;      /* rx_request */
				hfi_clean(rq, 48);
				break;
			}
			uint32_t resp[19] = {0}, hdr = rdata[ri], size = (hdr >> 8) & 0xff;
			if (size > 19) {
				printf("PANDORA GPU: HFI response size %u too large (hdr %08x)\n", size, hdr);
				return false;
			}
			for (uint32_t i = 0; i < size; i++) {
				resp[i] = rdata[ri];
				ri = (ri + 1) % HFI_QUEUE_DWORDS;
			}
			ri = ((ri + 3) & ~3u) % HFI_QUEUE_DWORDS;
			__asm__ volatile ("dsb sy" ::: "memory");
			rq[10] = ri;
			hfi_clean(rq, 48);
			if ((resp[0] & 0xff) == HFI_F2H_MSG_ERROR) {
				printf("PANDORA GPU: HFI firmware error packet: code %u (%08x %08x)\n", resp[1], resp[2],
				    resp[3]);
				continue;
			}
			uint32_t rseq = (resp[1] >> 20) & 0xfff;
			printf("PANDORA GPU: HFI response %08x for %08x error %u\n", resp[0], resp[1], resp[2]);
			if (rseq != seq) {
				continue;
			}
			return resp[2] == 0;
		}
	}
	return false;
}

/* a8xx_hfi_send_perf_table. */
static bool
gpu_level6(void)
{
	static uint16_t gfx[32], gmxc[32], mx[32], cx[32];
	unsigned ngfx = gpu_lvl("gfx.lvl", gfx, 32), ngmxc = gpu_lvl("gmxc.lvl", gmxc, 32);
	unsigned nmx = gpu_lvl("mx.lvl", mx, 32), ncx = gpu_lvl("cx.lvl", cx, 32);
	const uint16_t *sec = ngmxc ? gmxc : mx;
	unsigned nsec = ngmxc ? ngmxc : nmx;
	printf("PANDORA GPU: levels gfx %u gmxc %u mx %u cx %u\n", ngfx, ngmxc, nmx, ncx);
	if (!ngfx || !nmx || !ncx || !nsec) {
		return false;
	}
	static uint32_t msg[7 + 3 * NGPU + 2 * NGMU];
	uint32_t *g = msg + 5, *c;
	bzero(msg, sizeof(msg));
	msg[1] = 0;                     /* version */
	msg[2] = HFI_TABLE_GPU_PERF;
	msg[3] = NGPU;                  /* entry 0: GX, stride 3 */
	msg[4] = 3;
	for (unsigned i = 0; i < NGPU; i++) {
		uint32_t level = i ? gpu_opps[i - 1].level : 0, gxv, dep = 0xffffffffu;
		if (!arc_vote(level, gfx, ngfx, sec, nsec, &gxv)) {
			printf("PANDORA GPU: gfx level %#x not in cmd-db\n", level);
			return false;
		}
		if (i) {
			unsigned j;
			for (j = 0; j < nmx && mx[j] < level; j++) {
			}
			if (j == nmx) {
				printf("PANDORA GPU: mx level %#x not in cmd-db\n", level);
				return false;
			}
			dep = (0x3fffu << 14) | (j << 8) | 0xff;
		}
		g[3 * i] = gxv;
		g[3 * i + 1] = dep;
		g[3 * i + 2] = i ? gpu_opps[i - 1].hz / 1000 : 0;
	}
	c = g + 3 * NGPU;               /* entry 1: CX, stride 2 */
	c[0] = NGMU;
	c[1] = 2;
	for (unsigned i = 0; i < NGMU; i++) {
		uint32_t level = i ? gmu_opps[i - 1].level : 0, cxv;
		if (!arc_vote(level, cx, ncx, mx, nmx, &cxv)) {
			printf("PANDORA GPU: cx level %#x not in cmd-db\n", level);
			return false;
		}
		c[2 + 2 * i] = cxv;
		c[3 + 2 * i] = i ? gmu_opps[i - 1].hz / 1000 : 0;
	}
	printf("PANDORA GPU: perf table %u dwords: gx[1] %08x %08x %u, gx[%u] %08x %08x %u, cx[1] %08x %u, cx[%u] %08x %u\n",
	    (unsigned)(sizeof(msg) / 4), g[3], g[4], g[5], (unsigned)NGPU - 1, g[3 * (NGPU - 1)],
	    g[3 * (NGPU - 1) + 1], g[3 * (NGPU - 1) + 2], c[4], c[5], (unsigned)NGMU - 1, c[2 + 2 * (NGMU - 1)],
	    c[3 + 2 * (NGMU - 1)]);
	gpu_step("HFI: perf table (MSG_TABLE)");
	bool ok = hfi_send(HFI_H2F_MSG_TABLE, msg, (uint32_t)(sizeof(msg) / 4));
	printf("PANDORA GPU: HFI perf table %s\n", ok ? "ACKED" : "NOT ACKED");
	const uint32_t *log = (const uint32_t *)gmu_bufs[2].va;
	flush_dcache64((addr64_t)gmu_bufs[2].pa, 64, TRUE);
	printf("PANDORA GPU: GMU log %08x %08x %08x %08x %08x %08x %08x %08x; ao_host_irq %08x\n", log[0], log[1],
	    log[2], log[3], log[4], log[5], log[6], log[7], gmu_rd(GMU_AO_HOST_INTR_STATUS));
	return ok;
}

static void
gpu_thread(void *arg, wait_result_t result)
{
	int level = 0;
	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("pandora_gpu", &level, sizeof(level));
	printf("PANDORA GPU: level %d (1 = read only, 2 = CX power and SMMU clock, "
	    "3 = GPLL0 vote and GPU_INIT_REGS, 4 = GMU SMMU context, 5 = GMU cold start, 6 = HFI perf table)\n", level);
	gpu_cmddb();
	if (gpu_read_cc(true)) {
		gpu_read_smmu();
		gpu_read_gmu();
		if (level >= 2) {
			bool on = gpu_level2();
			printf("PANDORA GPU: level 2 %s\n", on ? "done" : "stopped");
			(void)gpu_read_cc(false);
			if (on) {
				gpu_read_smmu();
				gpu_read_gmu();
			}
			if (on && level >= 3) {
				bool ok = gpu_level3();
				printf("PANDORA GPU: level 3 %s\n", ok ? "done" : "SCM call failed");
				(void)gpu_read_cc(false);
				gpu_read_gmu();
				if (ok && level >= 4) {
					bool ctx = gpu_level4a();
					printf("PANDORA GPU: level 4a (GMU SMMU context) %s\n", ctx ? "done" : "stopped");
					gpu_read_gmu();
					if (ctx && level >= 5) {
						bool up = gpu_level5();
						printf("PANDORA GPU: level 5 (GMU cold start) %s\n", up ? "done" : "stopped");
						if (up && level >= 6) {
							bool t = gpu_level6();
							printf("PANDORA GPU: level 6 (HFI perf table) %s\n", t ? "done" : "stopped");
						}
					}
				}
			}
		}
	}
	printf("PANDORA GPU: done\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
pandora_gpu_init(void)
{
	thread_t thread;
	int level = 0;
	(void)PE_parse_boot_argn("pandora_gpu", &level, sizeof(level));
	if (level <= 0) {
		return;
	}
	if (kernel_thread_start(gpu_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */

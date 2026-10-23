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
 * (gpu6 log: the GMU acked the perf table, error 0.)
 *
 * Level 7 (patch 0168): the rest of a6xx_hfi_start: the bandwidth table HFI_H2F_MSG_BW_TABLE
 * (a6xx_generate_bw_table with a6xx_gmu_rpmh_bw_votes_init's BCM votes for a840's SH0/MC0/ACV,
 * CN0 for the CX path; the levels are the phone DT's qcom,bus-table-ddr, as KGSL builds them),
 * then HFI_H2F_MSG_CORE_FW_START and HFI_H2F_MSG_START. ACD and IFPC stay off.
 * (gpu7 log: all three acked; GX still off, PWR_CLK_STATUS 3.)
 *
 * Level 8 (patch 0169): GX on. The OOB GPU_SET handshake of a6xx_gmu_set_oob (non-legacy:
 * request bit 30, ack bit 31, ack cleared), then a6xx_hfi_set_freq's HFI_H2F_MSG_GX_BW_PERF_VOTE
 * with the lowest levels (perf index 1 = 160 MHz, bw index 1) where Linux starts at the top,
 * then a8xx_gmu_gx_is_on (PWR_CLK_STATUS & 3 == 0); only then RBBM_STATUS (A8XX 0x012) is
 * read from the GPU's own registers. a6xx_gmu_clear_oob (bit 31) at the end.
 * (gpu8 log: no OOB ack within 10 ms, GMU2HOST_INTR_INFO 0.) Patch 0170: Linux's order
 * instead: a6xx_gmu_resume, after a6xx_hfi_start, clears GMU2HOST_INTR, sets its mask to
 * ~CM3_FAULT and sends the GX vote (a6xx_gmu_set_initial_freq); the OOB GPU_SET comes later,
 * from a8xx hw_init. Here: interrupts, vote, then OOB with 100 ms (the GMU runs on XO).
 * (gpu8b log: vote acked, OOB acked, PWR_CLK_STATUS 0, RBBM_STATUS 1.)
 *
 * Level 9 (patch 0171): the GPU's own SMMU context (stream 0 = gfx3d_user, context bank 0 as
 * arm-smmu-qcom gives the Adreno, ACTLR 0x32b) with a ring, the SQE (gen80200_sqe.fw without
 * its first word, as adreno_fw_create_bo loads it) and a scratch page mapped; the SMMU code
 * is the level 4a routine made generic. Nothing is submitted yet.
 * (gpu9 log: bank 0 enabled, slot 3 = SID 0 -> bank 0, SQE loaded.)
 *
 * Level 10 (patch 0172): the CP. Linux a8xx hw_init's required part (aperture clear, GBIF and
 * RBBM GBIF halt clear, SECVID TSB off, CP_APRIV_CNTL_PIPE per pipe through
 * CP_APERTURE_CNTL_HOST, SQE and ring base, CP_RB_CNTL 0x20c, read-pointer copies for BR and
 * BV, CP_SQE_CNTL 1) under OOB GPU_SET, then a8xx_cp_init's CP_THREAD_CONTROL + CP_ME_INIT
 * (power-up list pointer at a zeroed page), the write pointer written as Linux's fenced write
 * (GMU_AHB_FENCE_STATUS bit 0 = dropped), idle wait; then one CP_MEM_WRITE while still secure
 * to see whether it lands. Values: Linux a8xx_gpu.c / a6xx.xml A8XX and Qualcomm gen8_reg.h.
 * (gpu10 log: CP_ME_INIT consumed, CP idle; CP_MEM_WRITE wrote 0xc0ffee00 even while secure.)
 *
 * Level 11 (patch 0173): leaving secure mode as a8xx hw_init does after cp_init: the zap shader
 * gen80200_zap.mbn as PAS image 13 (a6xx_zap_shader_init) into the phone DT's
 * gpu_microcode_region (0x9959a000, 8 KiB) through pandora_adsp.c's loader (metadata in
 * ordinary contiguous memory, no SHM bridge of our own), then CP_SET_SECURE_MODE 0 in the
 * ring and a second CP_MEM_WRITE.
 * (gpu11 log: init_image, mem_setup, auth_and_reset all 0; secure mode off; 0x5ec0ff00 written.)
 *
 * Level 12 (patch 0174): commands from a separate buffer as Mesa submits them: a page mapped at
 * GPU address 0x400000 holding one CP_MEM_WRITE, called from the ring with CP_INDIRECT_BUFFER;
 * then the BR and BV copies of CP_IB1_BASE / REM_SIZE / INIT_SIZE.
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

static ArmSmmu4KMmio smmu_mmio;
static ArmSmmu4KIo smmu_io;

/* One context bank with its tables and one routed stream; lives for the rest of the boot. */
struct smmu_ctx {
	ArmSmmu4KConfig tcfg;
	ArmSmmu4KResult tables;
	ArmSmmu4KContextConfig ctx;
	ArmSmmu4KProgramResult prog;
	ArmSmmu4KStreamState streams;
	ArmSmmu4KStreamResult sres;
};
static struct smmu_ctx gmu_smmu, gpu_smmu;

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

static vm_offset_t gpu_smmu_va;

/* Allocate the buffers (physically contiguous, zeroed) and log where they are. */
static bool
gpu_alloc_bufs(const char *who, struct gmu_buf *bufs, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		struct gmu_buf *b = &bufs[i];
		b->va = gpu_contig(b->bytes, &b->pa);
		if (!b->va) {
			printf("PANDORA GPU: %s %s: no %#llx contiguous bytes\n", who, b->name, b->bytes);
			return false;
		}
		printf("PANDORA GPU: %s %s: iova %#llx bytes %#llx pa %#llx\n", who, b->name, b->iova, b->bytes, b->pa);
	}
	return true;
}

/* Tables for bufs, context bank BANK (ACTLR, programmed, read back, enabled) and stream SID
 * routed to it through stream-match slot SLOT, all through ArmSmmu4K. */
static bool
smmu_ctx_up(const char *who, struct smmu_ctx *c, struct gmu_buf *bufs, unsigned n, unsigned bank,
    unsigned asid, unsigned slot, unsigned sid)
{
	uint64_t tpa;
	vm_offset_t tva = gpu_contig(GMU_TABLE_PAGES * 0x1000ull, &tpa);
	if (!tva) {
		printf("PANDORA GPU: %s tables: no memory\n", who);
		return false;
	}

	/* SMMU window and what the context encoding needs from it. */
	if (!gpu_smmu_va) {
		gpu_smmu_va = ml_io_map(SMMU_PA, SMMU_SIZE);
		if (!gpu_smmu_va) {
			printf("PANDORA GPU: SMMU window not mapped\n");
			return false;
		}
		smmu_mmio.mapping = (volatile unsigned char *)gpu_smmu_va;
		smmu_mmio.bytes = SMMU_SIZE;
		if (ArmSmmu4KBindMmio(&smmu_mmio, &smmu_io) != ArmSmmu4KSuccess) {
			return false;
		}
	}
	vm_offset_t smmu = gpu_smmu_va;
	uint32_t id0 = gpu_rd(smmu, SMMU_ID0), id1 = gpu_rd(smmu, SMMU_ID1), id2 = gpu_rd(smmu, SMMU_ID2);
	static const uint8_t oas_bits[16] = {32, 36, 40, 42, 44, 48};
	unsigned oas = ((id2 >> 4) & 15) < 6 ? oas_bits[(id2 >> 4) & 15] : 0;
	if (oas > 40) {
		oas = 40;
	}

	/* Tables. */
	ArmSmmu4KRange ranges[8];
	uint64_t leaves = 0;
	if (n > 8) {
		return false;
	}
	for (unsigned i = 0; i < n; i++) {
		ranges[i] = (ArmSmmu4KRange){ bufs[i].iova, bufs[i].pa, bufs[i].bytes, bufs[i].perm, 0, 2 };
		leaves += bufs[i].bytes / 0x1000;
		if ((bufs[i].pa >> oas) != 0) {
			printf("PANDORA GPU: %s %s above the %u-bit output size\n", who, bufs[i].name, oas);
			return false;
		}
	}
	c->tcfg = (ArmSmmu4KConfig){ 32, oas, tpa, (size_t)leaves };
	ArmSmmu4KStatus st = ArmSmmu4KBuild(&c->tcfg, ranges, n, (uint64_t *)tva, GMU_TABLE_PAGES, &c->tables);
	printf("PANDORA GPU: %s tables: status %d root %#llx pages %zu leaves %zu start level %u mair %#llx "
	    "(oas %u, id0 %08x id1 %08x id2 %08x)\n", who, st, c->tables.root_physical, c->tables.table_pages,
	    c->tables.leaf_pages, c->tables.start_level, c->tables.mair, oas, id0, id1, id2);
	if (st != ArmSmmu4KSuccess) {
		return false;
	}
	clean_dcache64((addr64_t)tpa, (unsigned)(c->tables.table_pages * 0x1000), TRUE);

	/* Context bank: must be disabled; ACTLR is the platform's part (the kit does not write it). */
	uint64_t page = (id1 & (1u << 31)) ? 0x10000 : 0x1000;
	uint64_t numpage = 1ull << (((id1 >> 28) & 7) + 1);
	uint64_t cb = numpage * page + bank * page;
	if (cb + 0x1000 > SMMU_SIZE) {
		printf("PANDORA GPU: context bank %u at %#llx outside the window\n", bank, cb);
		return false;
	}
	uint32_t sctlr = gpu_rd(smmu, (uint32_t)cb);
	printf("PANDORA GPU: SMMU bank %u at %#llx: sctlr %08x actlr %08x cbar %08x\n", bank, cb, sctlr,
	    gpu_rd(smmu, (uint32_t)cb + 4), gpu_rd(smmu, (uint32_t)(page + 4 * bank)));
	if (sctlr & 1) {
		printf("PANDORA GPU: bank %u already enabled: stop\n", bank);
		return false;
	}
	gpu_step("SMMU: ACTLR, then program and enable the context");
	gpu_wr(smmu, (uint32_t)cb + 4, GMU_ACTLR);
	c->ctx = (ArmSmmu4KContextConfig){ &c->tcfg, NULL, &c->tables, NULL, id0, id2, asid, 0, 0, 0 };
	st = ArmSmmu4KProgramDisabledContext(&smmu_io, bank, &c->ctx, 1000, &c->prog);
	printf("PANDORA GPU: %s SMMU context: status %d phase %u writes %u enabled %u fsr %08x actlr %08x sctlr %08x "
	    "tcr %08x ttbr0 %#llx mair0 %08x\n", who, st, c->prog.phase, c->prog.writes, c->prog.enabled,
	    c->prog.fault_status, gpu_rd(smmu, (uint32_t)cb + 4), c->prog.readback.sctlr, c->prog.readback.tcr,
	    c->prog.readback.ttbr0, c->prog.readback.mair0);
	if (st != ArmSmmu4KSuccess) {
		return false;
	}

	/* Stream: the free slot's S2CR must fault before the kit attaches it. */
	uint32_t smr = gpu_rd(smmu, SMMU_SMR(slot)), s2cr = gpu_rd(smmu, SMMU_S2CR(slot));
	printf("PANDORA GPU: SMMU slot %u before: smr %08x s2cr %08x\n", slot, smr, s2cr);
	if (smr & (1u << 31)) {
		printf("PANDORA GPU: slot %u in use: stop\n", slot);
		return false;
	}
	if (s2cr != (2u << 16)) {
		gpu_wr(smmu, SMMU_S2CR(slot), 2u << 16);
		printf("PANDORA GPU: slot %u S2CR set to fault: %08x\n", slot, gpu_rd(smmu, SMMU_S2CR(slot)));
	}
	gpu_step("SMMU: route the stream to the context");
	ArmSmmu4KStream route = { slot, (uint16_t)sid };
	st = ArmSmmu4KAttachStreams(&smmu_io, &c->streams, &route, 1, bank, &c->ctx, &c->sres);
	printf("PANDORA GPU: %s SMMU stream %u: status %d phase %u writes %u verified %u global fault %08x; "
	    "slot %u smr %08x s2cr %08x\n", who, sid, st, c->sres.phase, c->sres.writes, c->sres.streams_verified,
	    c->sres.global_fault, slot, gpu_rd(smmu, SMMU_SMR(slot)), gpu_rd(smmu, SMMU_S2CR(slot)));
	return st == ArmSmmu4KSuccess;
}

static bool
gpu_level4a(void)
{
	return gpu_alloc_bufs("GMU", gmu_bufs, GMU_NBUFS) &&
	       smmu_ctx_up("GMU", &gmu_smmu, gmu_bufs, GMU_NBUFS, GMU_BANK, GMU_ASID, GMU_SLOT, GMU_SID);
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
#define GMU_GMU2HOST_INTR_MASK    0x1f993u
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

/* ---- level 7: bandwidth table, CORE_FW_START, START ------------------------------------ */

#define HFI_H2F_MSG_BW_TABLE      3u
#define HFI_H2F_MSG_START         10u
#define HFI_H2F_MSG_CORE_FW_START 14u
#define BCM_CMD(commit, valid, x, y) \
	(((uint32_t)(commit) << 30) | ((uint32_t)(valid) << 29) | (((uint32_t)(x) & 0x3fff) << 14) | ((uint32_t)(y) & 0x3fff))

/* Phone DT qcom,kgsl-3d0 qcom,bus-table-ddr, KBps; the pwrlevels' qcom,bus-freq index it. */
static const uint32_t gpu_ddr_kbps[] = {
	0, 2136718, 5285156, 6074218, 6672899, 8171875, 10687500, 12449218, 14398437, 16500000, 18671875,
	20832031,
};
#define NBW (sizeof(gpu_ddr_kbps) / sizeof(gpu_ddr_kbps[0]))

/* a6xx_catalog.c a840 .bcms */
static const struct { const char *name; uint32_t buswidth; bool fixed; uint32_t perfmode; } a840_bcms[] = {
	{ "SH0", 16, false, 0 }, { "MC0", 4, false, 0 }, { "ACV", 0, true, 1u << 2 },
};
#define NBCM (sizeof(a840_bcms) / sizeof(a840_bcms[0]))

static bool
gpu_level7(void)
{
	/* a6xx_hfi_msg_bw_table: header, bw_level_num, cnoc_cmds_num, ddr_cmds_num, cnoc_wait_bitmask,
	 * ddr_wait_bitmask, cnoc_cmds_addrs[6], cnoc_cmds_data[2][6], ddr_cmds_addrs[8], ddr_cmds_data[16][8] */
	static uint32_t bw[160];
	uint32_t *cnoc_addrs = bw + 6, *cnoc_data = bw + 12, *ddr_addrs = bw + 24, *ddr_data = bw + 32;
	uint32_t unit[NBCM], width[NBCM], vcd[NBCM];
	bzero(bw, sizeof(bw));
	for (unsigned j = 0; j < NBCM; j++) {
		uint32_t addr, aux, len;
		if (cmddb_find(a840_bcms[j].name, &addr, &aux, &len) || len < 8) {
			printf("PANDORA GPU: cmd-db %s missing\n", a840_bcms[j].name);
			return false;
		}
		/* struct bcm_db { le32 unit; le16 width; u8 vcd; u8 reserved; } */
		unit[j] = db16(aux) | ((uint32_t)db16(aux + 2) << 16);
		width[j] = db16(aux + 4);
		vcd[j] = db16(aux + 6) & 0xff;
		ddr_addrs[j] = addr;
	}
	for (unsigned i = 0; i < NBW; i++) {
		for (unsigned j = 0; j < NBCM; j++) {
			bool commit = j == NBCM - 1 || vcd[j] != vcd[j + 1];
			uint32_t v;
			if (!gpu_ddr_kbps[i]) {
				v = BCM_CMD(commit, 0, 0, 0);
			} else if (a840_bcms[j].fixed) {
				v = BCM_CMD(commit, 1, 0, a840_bcms[j].perfmode);   /* a8xx: perfmode on every level */
			} else {
				uint64_t peak = (uint64_t)gpu_ddr_kbps[i] * width[j] / a840_bcms[j].buswidth;
				peak = peak * 1000 / unit[j];
				uint32_t vote = (uint32_t)MAX(1ull, MIN(peak, 0x3fffull));
				v = BCM_CMD(commit, 1, vote, vote);
			}
			ddr_data[8 * i + j] = v;
		}
	}
	bw[1] = NBW;
	bw[3] = NBCM;
	for (unsigned j = 0; j < NBCM; j++) {
		if (ddr_data[j] & (1u << 30)) {
			bw[5] |= 1u << j;
		}
	}
	uint32_t cn0, aux, len;
	if (cmddb_find("CN0", &cn0, &aux, &len)) {
		printf("PANDORA GPU: cmd-db CN0 missing\n");
		return false;
	}
	cnoc_addrs[0] = cn0;
	bw[2] = 1;
	cnoc_data[0] = BCM_CMD(1, 0, 0, 0);
	cnoc_data[6] = BCM_CMD(1, 1, 0, 1);
	bw[4] = 1;
	printf("PANDORA GPU: bw table: %u levels, ddr %05x %05x %05x wait %x, level 1 %08x %08x %08x, level %u "
	    "%08x %08x %08x, cnoc %05x %08x/%08x\n", bw[1], ddr_addrs[0], ddr_addrs[1], ddr_addrs[2], bw[5],
	    ddr_data[8], ddr_data[9], ddr_data[10], (unsigned)NBW - 1, ddr_data[8 * (NBW - 1)],
	    ddr_data[8 * (NBW - 1) + 1], ddr_data[8 * (NBW - 1) + 2], cn0, cnoc_data[0], cnoc_data[6]);

	gpu_step("HFI: bandwidth table, CORE_FW_START, START");
	bool ok = hfi_send(HFI_H2F_MSG_BW_TABLE, bw, 160);
	printf("PANDORA GPU: HFI bandwidth table %s\n", ok ? "ACKED" : "NOT ACKED");
	if (!ok) {
		return false;
	}
	static uint32_t fw_start[2], start[1];
	fw_start[1] = 0;        /* handle */
	ok = hfi_send(HFI_H2F_MSG_CORE_FW_START, fw_start, 2);
	printf("PANDORA GPU: HFI CORE_FW_START %s\n", ok ? "ACKED" : "NOT ACKED");
	if (!ok) {
		return false;
	}
	ok = hfi_send(HFI_H2F_MSG_START, start, 1);
	printf("PANDORA GPU: HFI START %s; gfx_pwr_clk_status %08x rpmh_power_state %08x ao_host_irq %08x\n",
	    ok ? "ACKED" : "NOT ACKED", gmu_rd(0x1f7e8), gmu_rd(0x1f7e9), gmu_rd(GMU_AO_HOST_INTR_STATUS));
	return ok;
}

/* ---- level 8: GX on ------------------------------------------------------------------ */

#define HFI_H2F_MSG_GX_BW_PERF_VOTE 30u
#define GMU_PWR_CLK_STATUS        0x1f7e8u     /* A8XX GMUCX_GFX_PWR_CLK_STATUS */
#define RBBM_STATUS_A8XX          0x012u

static bool
gpu_level8(void)
{
	bool ok;
	vm_offset_t gpu = ml_io_map(GPU_PA, 0x1000);
	if (!gpu) {
		printf("PANDORA GPU: GPU window not mapped\n");
		return false;
	}
	gpu_step("GMU: interrupts, GX vote (160 MHz, lowest bandwidth), OOB GPU_SET");
	/* a6xx_gmu_resume after a6xx_hfi_start: CM3 fault interrupt on, everything else masked */
	gmu_wr(GMU_GMU2HOST_INTR_CLR, ~0u);
	gmu_wr(GMU_GMU2HOST_INTR_MASK, ~(1u << 23));
	/* a6xx_gmu_set_initial_freq -> a6xx_hfi_set_freq */
	static uint32_t vote[4];
	vote[1] = 1;            /* ack_type: blocking */
	vote[2] = 1;            /* perf index: 160 MHz */
	vote[3] = 1;            /* bw index: 2136718 KBps */
	bool voted = hfi_send(HFI_H2F_MSG_GX_BW_PERF_VOTE, vote, 4);
	printf("PANDORA GPU: HFI GX vote %s; pwr_clk_status %08x intr info %08x\n", voted ? "ACKED" : "NOT ACKED",
	    gmu_rd(GMU_PWR_CLK_STATUS), gmu_rd(GMU_GMU2HOST_INTR_INFO));
	if (!voted) {
		return false;
	}
	/* a6xx_gmu_set_oob(GMU_OOB_GPU_SET), non-legacy bits, as a8xx hw_init starts */
	gmu_wr(GMU_HOST2GMU_INTR_SET, 1u << 30);
	uint32_t info = gpu_poll(gpu_gmu, GMU_REG(GMU_GMU2HOST_INTR_INFO), 1u << 31, 1u << 31, 100000, &ok);
	gmu_wr(GMU_GMU2HOST_INTR_CLR, 1u << 31);
	printf("PANDORA GPU: OOB GPU_SET %s (intr info %08x, pwr_clk_status %08x)\n", ok ? "acked" : "NOT ACKED",
	    info, gmu_rd(GMU_PWR_CLK_STATUS));
	if (!ok) {
		return false;
	}
	/* a8xx_gmu_gx_is_on */
	uint32_t st = gpu_poll(gpu_gmu, GMU_REG(GMU_PWR_CLK_STATUS), 3, 0, 100000, &ok);
	printf("PANDORA GPU: GX %s: pwr_clk_status %08x rpmh_power_state %08x ao_host_irq %08x\n",
	    ok ? "ON" : "still off", st, gmu_rd(0x1f7e9), gmu_rd(GMU_AO_HOST_INTR_STATUS));
	if (ok) {
		gpu_step("GPU: RBBM_STATUS");
		printf("PANDORA GPU: RBBM_STATUS %08x\n", gpu_rd(gpu, 4 * RBBM_STATUS_A8XX));
	}
	/* a6xx_gmu_clear_oob(GMU_OOB_GPU_SET) */
	gmu_wr(GMU_HOST2GMU_INTR_SET, 1u << 31);
	return voted && ok;
}

/* ---- level 9: the GPU's own SMMU context, ring, SQE, scratch ----------------------------- */

#define GPU_SID           0u           /* DT qcom,kgsl-iommu gfx3d_user iommus = <&kgsl_smmu 0x0 0x0> */
#define GPU_BANK          0u           /* arm-smmu-qcom gives the Adreno its context bank 0 */
#define GPU_ASID          2u
#define GPU_SLOT          3u

/* GPU addresses of the first command test; the SQE is the firmware without its first word
 * (adreno_fw_create_bo), mapped read-only and executable as Linux maps it. */
static struct gmu_buf gpu_bufs[] = {
	{ "ring", 0x00100000u, 0x8000u, ArmSmmu4KRead | ArmSmmu4KWrite, 0, 0 },
	{ "sqe", 0x00200000u, 0x20000u, ArmSmmu4KRead | ArmSmmu4KExecute, 0, 0 },
	{ "scratch", 0x00300000u, 0x1000u, ArmSmmu4KRead | ArmSmmu4KWrite, 0, 0 },
	{ "ib", 0x00400000u, 0x1000u, ArmSmmu4KRead | ArmSmmu4KWrite, 0, 0 },
};
#define GPU_NBUFS (sizeof(gpu_bufs) / sizeof(gpu_bufs[0]))

static bool
gpu_level9(void)
{
	uint64_t len = 0;
	uint8_t *sqe = gpu_fw_file("gen80200_sqe.fw", &len);
	if (!sqe) {
		return false;
	}
	if (len <= 4 || len - 4 > gpu_bufs[1].bytes) {
		printf("PANDORA GPU: sqe size %llu does not fit\n", len);
		kfree_data(sqe, (vm_size_t)len);
		return false;
	}
	if (!gpu_alloc_bufs("GPU", gpu_bufs, GPU_NBUFS)) {
		kfree_data(sqe, (vm_size_t)len);
		return false;
	}
	memcpy((void *)gpu_bufs[1].va, sqe + 4, (size_t)(len - 4));
	clean_dcache64((addr64_t)gpu_bufs[1].pa, (unsigned)gpu_bufs[1].bytes, TRUE);
	uint32_t w[4];
	memcpy(w, sqe + 4, sizeof(w));
	printf("PANDORA GPU: SQE %llu bytes loaded at iova %#llx: %08x %08x %08x %08x\n", len - 4, gpu_bufs[1].iova,
	    w[0], w[1], w[2], w[3]);
	kfree_data(sqe, (vm_size_t)len);
	return smmu_ctx_up("GPU", &gpu_smmu, gpu_bufs, GPU_NBUFS, GPU_BANK, GPU_ASID, GPU_SLOT, GPU_SID);
}

/* ---- level 10: the CP ------------------------------------------------------------------- */

/* A8XX GPU registers, dword offsets (a6xx.xml A8XX variants = gen8_reg.h) */
#define RBBM_GBIF_HALT            0x00au
#define RBBM_INT_0_STATUS         0x06au
#define CP_RB_BASE                0x800u
#define CP_RB_CNTL                0x802u
#define CP_RB_RPTR_ADDR           0x804u
#define CP_RB_RPTR                0x806u
#define CP_RB_WPTR                0x807u
#define CP_RB_RPTR_ADDR_BV        0x808u
#define CP_RB_RPTR_BV             0x80au
#define CP_SQE_CNTL               0x815u
#define CP_SQE_INSTR_BASE         0x816u
#define CP_APERTURE_CNTL_HOST     0x81cu
#define CP_APRIV_CNTL_PIPE        0x84du
#define CP_HW_FAULT_STATUS_PIPE   0x8b3u
#define CP_INTERRUPT_STATUS_GLOBAL 0x8b5u
#define CP_INTERRUPT_STATUS_PIPE  0x8b7u
#define CP_SQE_STATUS_PIPE        0x8bbu
#define GBIF_HALT                 0x3c45u
#define RBBM_SECVID_TSB_TRUSTED_BASE 0xf800u
#define RBBM_SECVID_TSB_TRUSTED_SIZE 0xf802u
#define RBBM_SECVID_TSB_CNTL      0xf803u
#define GMU_AHB_FENCE_STATUS      0x23b13u
#define GMU_AO_GPU_CX_BUSY_STATUS 0x23b0cu

static vm_offset_t gpu_regs;
static uint32_t ring_wptr;

static uint32_t
gr(uint32_t dw)
{
	return gpu_rd(gpu_regs, 4 * dw);
}

static void
gw(uint32_t dw, uint32_t v)
{
	gpu_wr(gpu_regs, 4 * dw, v);
}

/* adreno_pm4: PKT7 header with the odd parity bits of a6xx_gpu PM4_PARITY. */
static uint32_t
pm4_parity(uint32_t v)
{
	return (0x9669u >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^
	    (v >> 28)))) & 1;
}

static uint32_t
pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | (pm4_parity(cnt) << 15) | ((op & 0x7f) << 16) | (pm4_parity(op) << 23);
}

static void
ring_put(uint32_t v)
{
	((uint32_t *)gpu_bufs[0].va)[ring_wptr] = v;
	ring_wptr = (ring_wptr + 1) % (uint32_t)(gpu_bufs[0].bytes / 4);
}

/* a6xx_flush + fenced write: publish the ring, write WPTR, retry while the GMU fence drops it. */
static void
ring_kick(void)
{
	clean_dcache64((addr64_t)gpu_bufs[0].pa, (unsigned)gpu_bufs[0].bytes, TRUE);
	__asm__ volatile ("dsb sy" ::: "memory");
	unsigned tries = 0;
	do {
		gw(CP_RB_WPTR, ring_wptr);
		__asm__ volatile ("dsb sy" ::: "memory");
		if (!(gmu_rd(GMU_AHB_FENCE_STATUS) & 1)) {
			break;
		}
		delay(10);
	} while (++tries < 200);
	printf("PANDORA GPU: WPTR %u written%s (fence status %08x)\n", ring_wptr, tries ? " after retries" : "",
	    gmu_rd(GMU_AHB_FENCE_STATUS));
}

/* Until the CP has read the ring up to WPTR and the GPU is idle (a8xx_idle), at most ms. */
static bool
ring_idle(unsigned ms)
{
	uint32_t rptr = 0, st = 0;
	for (unsigned t = 0; t < ms * 10; t++) {
		rptr = gr(CP_RB_RPTR);
		st = gr(RBBM_STATUS_A8XX);
		if (rptr == ring_wptr && !(st & ~1u)) {
			break;
		}
		delay(100);
	}
	flush_dcache64((addr64_t)gpu_bufs[2].pa, 0x200, TRUE);
	const uint32_t *scratch = (const uint32_t *)gpu_bufs[2].va;
	printf("PANDORA GPU: rptr %u (copy %u, bv %u) wptr %u, rbbm_status %08x int %08x cx_busy %08x, "
	    "sqe status %08x cp int %08x\n", rptr, scratch[0x100 / 4], gr(CP_RB_RPTR_BV), ring_wptr, st,
	    gr(RBBM_INT_0_STATUS), gmu_rd(GMU_AO_GPU_CX_BUSY_STATUS), gr(CP_SQE_STATUS_PIPE),
	    gr(CP_INTERRUPT_STATUS_GLOBAL));
	return rptr == ring_wptr && !(st & ~1u);
}

static bool
gpu_level10(void)
{
	bool ok;
	gpu_regs = ml_io_map(GPU_PA, 0x40000);
	if (!gpu_regs) {
		printf("PANDORA GPU: GPU registers not mapped\n");
		return false;
	}
	/* a6xx_gmu_set_oob(GMU_OOB_GPU_SET), held for the whole level */
	gmu_wr(GMU_HOST2GMU_INTR_SET, 1u << 30);
	(void)gpu_poll(gpu_gmu, GMU_REG(GMU_GMU2HOST_INTR_INFO), 1u << 31, 1u << 31, 100000, &ok);
	gmu_wr(GMU_GMU2HOST_INTR_CLR, 1u << 31);
	printf("PANDORA GPU: OOB GPU_SET %s, pwr_clk_status %08x\n", ok ? "acked" : "NOT ACKED", gmu_rd(GMU_PWR_CLK_STATUS));
	if (!ok || (gmu_rd(GMU_PWR_CLK_STATUS) & 3)) {
		return false;
	}

	gpu_step("CP: hw_init registers, SQE start");
	gw(CP_APERTURE_CNTL_HOST, 0);
	gw(GBIF_HALT, 0);
	(void)gr(GBIF_HALT);
	gw(RBBM_GBIF_HALT, 0);
	(void)gr(RBBM_GBIF_HALT);
	gw(RBBM_SECVID_TSB_CNTL, 0);
	gw(RBBM_SECVID_TSB_TRUSTED_BASE, 0);
	gw(RBBM_SECVID_TSB_TRUSTED_BASE + 1, 0);
	gw(RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
	/* Pipes BR 1, BV 2, AQE0 4, AQE1 5, DDE_BR 6, DDE_BV 7 (LPAC 3 skipped); BR adds CDREAD/CDWRITE. */
	static const uint32_t pipes[] = { 1, 2, 4, 5, 6, 7 };
	for (unsigned i = 0; i < sizeof(pipes) / sizeof(pipes[0]); i++) {
		gw(CP_APERTURE_CNTL_HOST, pipes[i] << 12);
		gw(CP_APRIV_CNTL_PIPE, pipes[i] == 1 ? 0x6f : 0x0f);
	}
	gw(CP_APERTURE_CNTL_HOST, 1u << 12);
	uint32_t apriv_br = gr(CP_APRIV_CNTL_PIPE);
	gw(CP_APERTURE_CNTL_HOST, 0);
	gw(CP_SQE_INSTR_BASE, (uint32_t)gpu_bufs[1].iova);
	gw(CP_SQE_INSTR_BASE + 1, 0);
	gw(CP_RB_BASE, (uint32_t)gpu_bufs[0].iova);
	gw(CP_RB_BASE + 1, 0);
	gw(CP_RB_CNTL, 0x20c);
	gw(CP_RB_RPTR_ADDR, (uint32_t)gpu_bufs[2].iova + 0x100);
	gw(CP_RB_RPTR_ADDR + 1, 0);
	gw(CP_RB_RPTR_ADDR_BV, (uint32_t)gpu_bufs[2].iova + 0x108);
	gw(CP_RB_RPTR_ADDR_BV + 1, 0);
	printf("PANDORA GPU: CP regs: gbif halt %08x/%08x apriv br %08x sqe base %08x rb base %08x cntl %08x "
	    "rptr addr %08x/%08x\n", gr(GBIF_HALT), gr(RBBM_GBIF_HALT), apriv_br, gr(CP_SQE_INSTR_BASE),
	    gr(CP_RB_BASE), gr(CP_RB_CNTL), gr(CP_RB_RPTR_ADDR), gr(CP_RB_RPTR_ADDR_BV));
	gw(CP_SQE_CNTL, 1);
	delay(1000);
	printf("PANDORA GPU: SQE started: sqe status %08x rbbm_status %08x cp int %08x pipe int %08x hw fault %08x\n",
	    gr(CP_SQE_STATUS_PIPE), gr(RBBM_STATUS_A8XX), gr(CP_INTERRUPT_STATUS_GLOBAL), gr(CP_INTERRUPT_STATUS_PIPE),
	    gr(CP_HW_FAULT_STATUS_PIPE));

	/* a8xx_cp_init */
	gpu_step("CP: CP_THREAD_CONTROL + CP_ME_INIT");
	ring_wptr = 0;
	ring_put(pkt7(0x17, 1));                /* CP_THREAD_CONTROL */
	ring_put(0x08000000);                   /* concurrent binning off */
	ring_put(pkt7(0x48, 7));                /* CP_ME_INIT */
	ring_put(0x0000014b);                   /* feature mask */
	ring_put(0x00000003);                   /* multiple hardware contexts */
	ring_put(0x20000000);                   /* error detection */
	ring_put(0x00000002);                   /* operation mode */
	ring_put((uint32_t)gpu_bufs[2].iova + 0x800);   /* power-up list (zeroed) */
	ring_put(0);
	ring_put(0x80000000);                   /* triplet list format */
	printf("PANDORA GPU: ring %08x %08x %08x %08x ...\n", ((uint32_t *)gpu_bufs[0].va)[0],
	    ((uint32_t *)gpu_bufs[0].va)[1], ((uint32_t *)gpu_bufs[0].va)[2], ((uint32_t *)gpu_bufs[0].va)[3]);
	ring_kick();
	bool init = ring_idle(1000);
	printf("PANDORA GPU: CP_ME_INIT %s\n", init ? "DONE (CP idle at WPTR)" : "NOT DONE");

	if (init) {
		gpu_step("CP: CP_MEM_WRITE while secure");
		ring_put(pkt7(0x3d, 3));        /* CP_MEM_WRITE */
		ring_put((uint32_t)gpu_bufs[2].iova);
		ring_put(0);
		ring_put(0xc0ffee00);
		ring_put(pkt7(0x12, 0));        /* CP_WAIT_MEM_WRITES */
		ring_kick();
		bool idle = ring_idle(1000);
		flush_dcache64((addr64_t)gpu_bufs[2].pa, 64, TRUE);
		printf("PANDORA GPU: CP_MEM_WRITE (secure) %s: scratch[0] = %08x\n", idle ? "consumed" : "NOT consumed",
		    ((const uint32_t *)gpu_bufs[2].va)[0]);
	}
	/* a6xx_gmu_clear_oob(GMU_OOB_GPU_SET) */
	gmu_wr(GMU_HOST2GMU_INTR_SET, 1u << 31);
	return init;
}

/* ---- level 11: zap shader, out of secure mode ---------------------------------------------- */

extern int pandora_pas_load_auth(const char *file, unsigned pas, uint64_t mem_pa, uint64_t mem_size);
#define GPU_ZAP_PAS       13u
#define GPU_ZAP_PA        0x9959a000ull     /* phone DT gpu_microcode_region, no-map */
#define GPU_ZAP_SIZE      0x2000ull

static bool
gpu_level11(void)
{
	bool ok;
	gmu_wr(GMU_HOST2GMU_INTR_SET, 1u << 30);
	(void)gpu_poll(gpu_gmu, GMU_REG(GMU_GMU2HOST_INTR_INFO), 1u << 31, 1u << 31, 100000, &ok);
	gmu_wr(GMU_GMU2HOST_INTR_CLR, 1u << 31);
	if (!ok) {
		printf("PANDORA GPU: OOB GPU_SET NOT ACKED\n");
		return false;
	}
	gpu_step("zap shader: PAS 13 load and auth");
	int err = pandora_pas_load_auth("gen80200_zap.mbn", GPU_ZAP_PAS, GPU_ZAP_PA, GPU_ZAP_SIZE);
	printf("PANDORA GPU: zap shader (PAS %u at %#llx) -> %d\n", GPU_ZAP_PAS, GPU_ZAP_PA, err);
	bool secure_off = false, wrote = false;
	if (!err) {
		gpu_step("CP: CP_SET_SECURE_MODE 0, CP_MEM_WRITE");
		ring_put(pkt7(0x66, 1));                /* CP_SET_SECURE_MODE */
		ring_put(0);
		ring_kick();
		secure_off = ring_idle(1000);
		printf("PANDORA GPU: CP_SET_SECURE_MODE 0 %s\n", secure_off ? "consumed" : "NOT consumed");
		if (secure_off) {
			ring_put(pkt7(0x3d, 3));        /* CP_MEM_WRITE */
			ring_put((uint32_t)gpu_bufs[2].iova + 4);
			ring_put(0);
			ring_put(0x5ec0ff00);
			ring_put(pkt7(0x12, 0));        /* CP_WAIT_MEM_WRITES */
			ring_kick();
			bool idle = ring_idle(1000);
			flush_dcache64((addr64_t)gpu_bufs[2].pa, 64, TRUE);
			uint32_t v = ((const uint32_t *)gpu_bufs[2].va)[1];
			wrote = idle && v == 0x5ec0ff00;
			printf("PANDORA GPU: CP_MEM_WRITE (non-secure) %s: scratch[1] = %08x\n",
			    idle ? "consumed" : "NOT consumed", v);
		}
	}
	gmu_wr(GMU_HOST2GMU_INTR_SET, 1u << 31);
	return secure_off && wrote;
}

/* ---- level 12: an indirect buffer ------------------------------------------------------------ */

#define CP_IB1_BASE_PIPE          0x900u
#define CP_IB1_REM_SIZE_PIPE      0x902u
#define CP_IB1_INIT_SIZE_PIPE     0x903u

static bool
gpu_level12(void)
{
	bool ok;
	gmu_wr(GMU_HOST2GMU_INTR_SET, 1u << 30);
	(void)gpu_poll(gpu_gmu, GMU_REG(GMU_GMU2HOST_INTR_INFO), 1u << 31, 1u << 31, 100000, &ok);
	gmu_wr(GMU_GMU2HOST_INTR_CLR, 1u << 31);
	if (!ok) {
		printf("PANDORA GPU: OOB GPU_SET NOT ACKED\n");
		return false;
	}
	gpu_step("CP: CP_INDIRECT_BUFFER with a CP_MEM_WRITE inside");
	uint32_t *ib = (uint32_t *)gpu_bufs[3].va;
	ib[0] = pkt7(0x3d, 3);                  /* CP_MEM_WRITE */
	ib[1] = (uint32_t)gpu_bufs[2].iova + 8;
	ib[2] = 0;
	ib[3] = 0x1b000012;
	clean_dcache64((addr64_t)gpu_bufs[3].pa, 64, TRUE);
	ring_put(pkt7(0x3f, 3));                /* CP_INDIRECT_BUFFER */
	ring_put((uint32_t)gpu_bufs[3].iova);
	ring_put(0);
	ring_put(4);                            /* size in dwords */
	ring_put(pkt7(0x12, 0));                /* CP_WAIT_MEM_WRITES */
	ring_kick();
	bool idle = ring_idle(1000);
	flush_dcache64((addr64_t)gpu_bufs[2].pa, 64, TRUE);
	uint32_t v = ((const uint32_t *)gpu_bufs[2].va)[2];
	printf("PANDORA GPU: CP_INDIRECT_BUFFER %s: scratch[2] = %08x\n", idle ? "consumed" : "NOT consumed", v);
	/* What the CP kept of the IB, per pipe (BR 1, BV 2) through the host aperture. */
	for (uint32_t pipe = 1; pipe <= 2; pipe++) {
		gw(CP_APERTURE_CNTL_HOST, pipe << 12);
		printf("PANDORA GPU: %s IB1 base %08x%08x rem %08x init %08x\n", pipe == 1 ? "BR" : "BV",
		    gr(CP_IB1_BASE_PIPE + 1), gr(CP_IB1_BASE_PIPE), gr(CP_IB1_REM_SIZE_PIPE),
		    gr(CP_IB1_INIT_SIZE_PIPE));
	}
	gw(CP_APERTURE_CNTL_HOST, 0);
	gmu_wr(GMU_HOST2GMU_INTR_SET, 1u << 31);
	return idle && v == 0x1b000012;
}

static void
gpu_thread(void *arg, wait_result_t result)
{
	int level = 0;
	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("pandora_gpu", &level, sizeof(level));
	printf("PANDORA GPU: level %d (1 = read only, 2 = CX power and SMMU clock, "
	    "3 = GPLL0 vote and GPU_INIT_REGS, 4 = GMU SMMU context, 5 = GMU cold start, 6 = HFI perf table, 7 = HFI start, 8 = GX on, 9 = GPU SMMU context, 10 = CP, 11 = zap, 12 = indirect buffer)\n", level);
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
							if (t && level >= 7) {
								bool s = gpu_level7();
								printf("PANDORA GPU: level 7 (HFI start) %s\n", s ? "done" : "stopped");
								if (s && level >= 8) {
									bool gx = gpu_level8();
									printf("PANDORA GPU: level 8 (GX on) %s\n", gx ? "done" : "stopped");
									if (gx && level >= 9) {
										bool m = gpu_level9();
										printf("PANDORA GPU: level 9 (GPU SMMU context, SQE) %s\n",
										    m ? "done" : "stopped");
										if (m && level >= 10) {
											bool cp = gpu_level10();
											printf("PANDORA GPU: level 10 (CP) %s\n", cp ? "done" : "stopped");
											if (cp && level >= 11) {
												bool z = gpu_level11();
												printf("PANDORA GPU: level 11 (zap, non-secure) %s\n",
												    z ? "done" : "stopped");
												if (z && level >= 12) {
													bool i = gpu_level12();
													printf("PANDORA GPU: level 12 (indirect buffer) %s\n",
													    i ? "done" : "stopped");
												}
											}
										}
									}
								}
							}
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

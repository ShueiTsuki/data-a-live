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
 * (gpu12 log: IB ran on BR, BR IB1 base 0x400000 rem 0 init 4; BV IB1 all 0.)
 *
 * Level 13 (patch 0175): what the user interface will stand on. A contiguous buffer pool (1 GiB
 * asked, halved to 16 MiB on refusal), GPU mappings added to the live context afterwards (the
 * GPU context's table pool is now 4 MiB), and two submissions framed as Linux a7xx_submit and
 * NativeAdrenoRing do (context entry with SRAM init on the first, A8XX registers from gen8_reg.h)
 * with IBs in a pool buffer at 0x20000000, the first IB on BR, the second on BOTH; completion by
 * the ring timestamp in msm_rbmemptrs layout at scratch + 0x100.
 * (gpu13 log: both fences reached, both words written; but the pool, asked for at level 13's
 * turn while userland ran, got only 128 MiB: 1024/512/256 found no free run.)
 * Patch 0176: the pool is reserved in pandora_gpu_init, before launchd, as the myron driver
 * reserves it at start (1 GiB there on the same phone).
 *
 * Level 14 (patch 0177): the interface NativeAdreno's user client (and so Turnip's Darwin
 * backend) needs, in pandora_gpu_* functions: buffers from the pool (best-fit extents),
 * mappings at client-chosen addresses inside per-client windows, CPU views by token, framed
 * submissions of up to 16 IBs, fence waits, the always-on counter. The IOKit side is
 * iokit/Kernel/arm/PandoraAdrenoGpu.cpp. Level 14 holds OOB GPU_SET for good and runs one
 * kernel self-test through these functions. Level 13's test buffer moved to 0x1f000000.
 * (gpu14 / pgpu1 logs: 1 GiB pool before launchd; tools/pgputest PASS from user space, but every
 * CPU view logged IOKit's "IOMemoryDescriptor physical with managed page".)
 * Patch 0178: the pool comes from RAM the loader keeps from XNU when the ADT has
 * pandora-gpu-pool (mkadt_pandora.py --gpu-pool, boot_ios.sh GPUPOOL=MiB); the kernel-memory pool
 * stays as the fallback. (pgpu2 log: mapping the carved 1 GiB at once panicked in io_map.)
 * Patch 0179: the kernel maps carved-pool windows write-combined only while it touches them.
 * (pgpu3 log: unmapping an ml_io_map_wcomb window panicked, those entries are permanent.)
 * Patch 0180: the windows come from ml_io_map_unmappable, which ml_io_unmap can take back.
 * (pgpu4 log: carved 1 GiB pool, self-test and 8 pgputest runs pass, no IOKit warnings.)
 *
 * Level 15 (patch 0181): the rest of Linux a8xx hw_init (GMEM range, UCHE trap, UBWC, the A840
 * non-context registers, CP protection, interrupt and fault masks, GMEM_PROTECT), run inside
 * level 14 before the user interface opens, then read back.
 * (gpu15 log: all read back as written except RB_CMP_NC_MODE_CNTL bits 1-2; tests pass.)
 * Patch 0182: the chip id given to user space carries the enabled slice count (0x44050a31).
 * Patch 0183: gpu_step no longer waits 3.5 s for the log flush unless pandora_gpu_slow=1.
 * Patch 0187: imported buffers (BoImport, an IOSurface's pages): mapped page by page with one TLB
 * invalidate, cleaned and invalidated from the CPU caches before each submission of their client.
 * (mtl2 log: Metal renders into an IOSurface, read back by the CPU, 10/10. bb1 log: backboardd as
 * a Metal client keeps its device, but the frame dumped at 130 s is black.)
 * Patch 0189: per-client counts and a watch log of them, the ring and SMMU faults (gpu_watch).
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
#include <kern/locks.h>
#include <sys/proc.h>
#include <pexpert/device_tree.h>
extern vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
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

/* Log what comes next; with boot-arg pandora_gpu_slow=1 also let the log thread write it before
 * the step runs (the bring-up default until patch 0183: 27 steps x 3.5 s held the user
 * interface back by over a minute once the bring-up stopped hanging). */
static void
gpu_step(const char *what)
{
	static int slow = -1;
	if (slow < 0) {
		slow = 0;
		(void)PE_parse_boot_argn("pandora_gpu_slow", &slow, sizeof(slow));
	}
	printf("PANDORA GPU: next: %s\n", what);
	if (slow) {
		gpu_sleep(3500);
	}
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
#define GPU_TABLE_PAGES   1024u     /* 4 MiB: the level-9 tables plus room for ~2 GiB of later 4K maps */

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
	vm_offset_t tva;                /* table pool: kernel address, size in pages, next unused page */
	unsigned tpages, tnext;
	unsigned bank;
	uint32_t cb;                    /* the context bank's page in the SMMU window (FSR at +0x58) */
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
    unsigned asid, unsigned slot, unsigned sid, unsigned tpages)
{
	uint64_t tpa;
	vm_offset_t tva = gpu_contig(tpages * 0x1000ull, &tpa);
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
	ArmSmmu4KStatus st = ArmSmmu4KBuild(&c->tcfg, ranges, n, (uint64_t *)tva, tpages, &c->tables);
	printf("PANDORA GPU: %s tables: status %d root %#llx pages %zu leaves %zu start level %u mair %#llx "
	    "(oas %u, id0 %08x id1 %08x id2 %08x)\n", who, st, c->tables.root_physical, c->tables.table_pages,
	    c->tables.leaf_pages, c->tables.start_level, c->tables.mair, oas, id0, id1, id2);
	if (st != ArmSmmu4KSuccess) {
		return false;
	}
	clean_dcache64((addr64_t)tpa, (unsigned)(c->tables.table_pages * 0x1000), TRUE);
	c->tva = tva;
	c->tpages = tpages;
	c->tnext = (unsigned)c->tables.table_pages;
	c->bank = bank;

	/* Context bank: must be disabled; ACTLR is the platform's part (the kit does not write it). */
	uint64_t page = (id1 & (1u << 31)) ? 0x10000 : 0x1000;
	uint64_t numpage = 1ull << (((id1 >> 28) & 7) + 1);
	uint64_t cb = numpage * page + bank * page;
	c->cb = (uint32_t)cb;
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
	       smmu_ctx_up("GMU", &gmu_smmu, gmu_bufs, GMU_NBUFS, GMU_BANK, GMU_ASID, GMU_SLOT, GMU_SID,
	           GMU_TABLE_PAGES);
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
	gpu_step("GMU: interrupts, GX vote (pandora_gpu_perf / pandora_gpu_bw, lowest by default), OOB GPU_SET");
	/* a6xx_gmu_resume after a6xx_hfi_start: CM3 fault interrupt on, everything else masked */
	gmu_wr(GMU_GMU2HOST_INTR_CLR, ~0u);
	gmu_wr(GMU_GMU2HOST_INTR_MASK, ~(1u << 23));
	/* a6xx_gmu_set_initial_freq -> a6xx_hfi_set_freq */
	static uint32_t vote[4];
	/* 0196: boot-args pandora_gpu_perf (perf index 1..18 into gpu_opps, default 1 = 160 MHz) and
	 * pandora_gpu_bw (bw index 1..11 into gpu_ddr_kbps, default 1). bb22: frames of ~100 draws took
	 * 10-15 ms at the lowest level; there is no DCVS yet, the vote holds for the whole boot. */
	uint32_t perf = 1, bwi = 1;
	(void)PE_parse_boot_argn("pandora_gpu_perf", &perf, sizeof(perf));
	(void)PE_parse_boot_argn("pandora_gpu_bw", &bwi, sizeof(bwi));
	perf = MAX(1u, MIN(perf, (uint32_t)NGPU - 1));
	bwi = MAX(1u, MIN(bwi, (uint32_t)NBW - 1));
	vote[1] = 1;            /* ack_type: blocking */
	vote[2] = perf;         /* perf index */
	vote[3] = bwi;          /* bw index */
	bool voted = hfi_send(HFI_H2F_MSG_GX_BW_PERF_VOTE, vote, 4);
	printf("PANDORA GPU: HFI GX vote %u MHz, %u KBps %s; pwr_clk_status %08x intr info %08x\n",
	    gpu_opps[perf - 1].hz / 1000000, gpu_ddr_kbps[bwi], voted ? "ACKED" : "NOT ACKED",
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
	return smmu_ctx_up("GPU", &gpu_smmu, gpu_bufs, GPU_NBUFS, GPU_BANK, GPU_ASID, GPU_SLOT, GPU_SID,
	    GPU_TABLE_PAGES);
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
#define CP_IB2_BASE_PIPE          0x904u
#define CP_IB2_REM_SIZE_PIPE      0x906u
#define CP_IB2_INIT_SIZE_PIPE     0x907u
#define CP_IB3_BASE_PIPE          0x908u

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

/* ---- level 13: buffer pool, later GPU mappings, a framed submission --------------------------- */

/* Map [iova, iova + bytes) to pa in a live context's tables, 4K leaves in the ArmSmmu4K format
 * (normal non-cacheable, outer shareable, as NativeAdreno maps user buffers), table pages taken
 * from the rest of the context's pool, then TLBIALL for the bank. Refuses pages already mapped. */
static bool
smmu_map_pages(struct smmu_ctx *c, uint64_t iova, uint64_t pa, const uint64_t *pages, uint64_t bytes,
    unsigned perm);
static void smmu_unmap(struct smmu_ctx *c, uint64_t iova, uint64_t bytes);

static bool
smmu_map(struct smmu_ctx *c, uint64_t iova, uint64_t pa, uint64_t bytes, unsigned perm)
{
	return smmu_map_pages(c, iova, pa, NULL, bytes, perm);
}

/* PAGES, when given, holds the physical address of each 4K page (an imported buffer, patch 0187);
 * otherwise the range is PA onwards. A failure takes back the leaves this call wrote. */
static bool
smmu_map_pages(struct smmu_ctx *c, uint64_t iova, uint64_t pa, const uint64_t *pages, uint64_t bytes,
    unsigned perm)
{
	uint64_t pool = c->tables.root_physical, attr = (1u << 11) | (1u << 10) | 3 | (2u << 8);
	unsigned in = c->tcfg.input_bits, start = c->tables.start_level;
	if (((iova | pa | bytes) & 0xfff) || !bytes || iova + bytes > (1ull << in) || !(perm & ArmSmmu4KRead)) {
		return false;
	}
	if (!(perm & ArmSmmu4KExecute)) {
		attr |= 3ull << 53;
	}
	attr |= 1u << 6;                        /* unprivileged, as Build's default */
	if (!(perm & ArmSmmu4KWrite)) {
		attr |= 1u << 7;
	}
	for (uint64_t off = 0; off < bytes; off += 0x1000) {
		uint64_t va = iova + off, page = pages ? pages[off >> 12] : pa + off;
		uint64_t *t = (uint64_t *)c->tva;      /* root page */
		bool ok = (page & 0xfff) == 0;
		for (unsigned level = start; level < 3 && ok; level++) {
			unsigned shift = 39 - 9 * level, width = level == start ? in - shift : 9;
			uint64_t *e = &t[(va >> shift) & ((1u << width) - 1)];
			if (!*e) {
				if (c->tnext >= c->tpages) {
					printf("PANDORA GPU: SMMU table pool full at iova %#llx\n", va);
					ok = false;
					break;
				}
				*e = (pool + 0x1000ull * c->tnext++) | 3;
				clean_dcache64((addr64_t)(pool + ((vm_offset_t)e - c->tva)), 8, TRUE);
			}
			t = (uint64_t *)(c->tva + ((*e & 0xfffffffff000ull) - pool));
		}
		uint64_t *leaf = ok ? &t[(va >> 12) & 511] : NULL;
		if (leaf && *leaf) {
			printf("PANDORA GPU: iova %#llx already mapped (%#llx)\n", va, *leaf);
			ok = false;
		}
		if (!ok) {
			if (off) {
				smmu_unmap(c, iova, off);
			}
			return false;
		}
		*leaf = page | attr;
		clean_dcache64((addr64_t)(pool + ((vm_offset_t)leaf - c->tva)), 8, TRUE);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	ArmSmmu4KProgramResult r;
	ArmSmmu4KStatus st = ArmSmmu4KInvalidateAllOwnedContext(&smmu_io, c->bank, &c->ctx, 1000, &r);
	if (st != ArmSmmu4KSuccess) {
		printf("PANDORA GPU: TLB invalidate for bank %u: status %d\n", c->bank, st);
		return false;
	}
	return true;
}

/* The buffer pool: physically contiguous, 1 GiB asked (myron: big, before anything else runs),
 * halved down to 16 MiB as NativeAdreno's GpuContext does when the allocator refuses. */
#define GPU_POOL_WANT     (1024ull << 20)
#define GPU_POOL_MIN      (16ull << 20)
static vm_offset_t gpu_pool_va;
static uint64_t gpu_pool_pa, gpu_pool_bytes;
static bool gpu_pool_carved;    /* RAM the loader kept from XNU (ADT pandora-gpu-pool), mapped write-combined */
#define GPU_POOL_EXTENTS  512u
static uint64_t gpu_ext_off[GPU_POOL_EXTENTS], gpu_ext_len[GPU_POOL_EXTENTS];
static unsigned gpu_next;

/*
 * Clean (and with INVAL also invalidate) the CPU caches over pool bytes [off, off + len). Nothing
 * to do for the carved pool: its only kernel mapping is write-combined (no cache lines), and its
 * pages are outside the physical aperture that the by-address cache operations go through.
 */
static void
gpu_pool_sync(uint64_t off, uint64_t len, bool inval)
{
	if (gpu_pool_carved) {
		__asm__ volatile ("dsb sy" ::: "memory");
	} else if (inval) {
		flush_dcache64((addr64_t)(gpu_pool_pa + off), (unsigned)len, TRUE);
	} else {
		clean_dcache64((addr64_t)(gpu_pool_pa + off), (unsigned)len, TRUE);
	}
}

/* A kernel view of pool bytes [off, off + len): the kernel-memory pool's own mapping, or for the
 * carved pool a write-combined window mapped now; give it back with gpu_pool_unmap. */
#define GPU_POOL_WINDOW   (4ull << 20)
static vm_offset_t
gpu_pool_map(uint64_t off, uint64_t len)
{
	if (!gpu_pool_carved) {
		return gpu_pool_va + (vm_offset_t)off;
	}
	/* The removable kind: ml_io_map_wcomb's io_submap entries are permanent and merge with their
	 * neighbours, so ml_io_unmap on them panics (pgpu3 log, vm_map_delete of a permanent entry). */
	uint64_t lead = off & 0xfff;
	vm_offset_t va = ml_io_map_unmappable((vm_offset_t)(gpu_pool_pa + off - lead),
	    (vm_size_t)((lead + len + 0xfff) & ~0xfffull), VM_WIMG_WCOMB);
	return va ? va + (vm_offset_t)lead : 0;
}

static void
gpu_pool_unmap(vm_offset_t va, uint64_t off, uint64_t len)
{
	if (gpu_pool_carved && va) {
		uint64_t lead = off & 0xfff;
		ml_io_unmap(va - (vm_offset_t)lead, (vm_size_t)((lead + len + 0xfff) & ~0xfffull));
	}
}

/* Zero pool bytes [off, off + len), a window at a time for the carved pool. */
static void
gpu_pool_zero(uint64_t off, uint64_t len)
{
	for (uint64_t at = 0; at < len; at += GPU_POOL_WINDOW) {
		uint64_t n = MIN(GPU_POOL_WINDOW, len - at);
		vm_offset_t va = gpu_pool_map(off + at, n);
		if (va) {
			bzero((void *)va, (size_t)n);
			gpu_pool_unmap(va, off + at, n);
		}
	}
	gpu_pool_sync(off, len, true);
}

static void
gpu_pool_ready(uint64_t bytes)
{
	gpu_pool_bytes = bytes;
	gpu_ext_off[0] = 0;
	gpu_ext_len[0] = bytes;
	gpu_next = 1;
	printf("PANDORA GPU: buffer pool at pa %#llx, %llu MiB%s\n", gpu_pool_pa, bytes >> 20,
	    gpu_pool_carved ? " (carved by the loader, write-combined)" : "");
}

/*
 * The pool. Preferably the RAM the loader carved off (mkadt_pandora.py --gpu-pool): pages XNU
 * never manages, so the user client's physical-address views of them are plain (IOKit warns
 * about such views of managed pages). Otherwise kernel memory, as before.
 */
static bool
gpu_pool_reserve(void)
{
	DTEntry chosen;
	const uint64_t *carve;
	unsigned int size;
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "pandora-gpu-pool", (void const **)&carve, &size) == kSuccess &&
	    size == 16 && carve[1]) {
		/* Not mapped as a whole: io_map has no 1 GiB of kernel addresses and panics
		 * (pgpu2 log, kmem_alloc 1073741824 failed); gpu_pool_map maps windows instead. */
		gpu_pool_pa = carve[0];
		gpu_pool_carved = true;
		gpu_pool_ready(carve[1]);
		return true;
	}
	for (uint64_t want = GPU_POOL_WANT; want >= GPU_POOL_MIN; want /= 2) {
		uint64_t t0 = mach_absolute_time(), ns;
		gpu_pool_va = gpu_contig(want, &gpu_pool_pa);
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		printf("PANDORA GPU: buffer pool %llu MiB: %s (%llu ms)\n", want >> 20,
		    gpu_pool_va ? "reserved" : "refused", ns / 1000000);
		if (gpu_pool_va) {
			gpu_pool_ready(want);
			return true;
		}
	}
	return false;
}

/* Best fit over a sorted list of free extents (pool offsets), merged on release, as
 * NativeAdrenoMemory does; ALIGN is a power of two, applied to the physical address. Returns the
 * pool offset or ~0. */
static uint64_t
gpu_pool_alloc(uint64_t bytes, uint64_t align)
{
	unsigned best = GPU_POOL_EXTENTS;
	uint64_t best_at = 0;
	for (unsigned i = 0; i < gpu_next; i++) {
		uint64_t at = ((gpu_pool_pa + gpu_ext_off[i] + align - 1) & ~(align - 1)) - gpu_pool_pa;
		if (at + bytes <= gpu_ext_off[i] + gpu_ext_len[i] &&
		    (best == GPU_POOL_EXTENTS || gpu_ext_len[i] < gpu_ext_len[best])) {
			best = i;
			best_at = at;
		}
	}
	if (best == GPU_POOL_EXTENTS) {
		return ~0ull;
	}
	uint64_t off = gpu_ext_off[best], end = off + gpu_ext_len[best];
	bool front = best_at > off, back = best_at + bytes < end;
	if (front && back && gpu_next == GPU_POOL_EXTENTS) {
		return ~0ull;
	}
	if (front) {
		gpu_ext_len[best] = best_at - off;
	}
	if (back) {
		unsigned at = front ? best + 1 : best;
		if (front) {
			memmove(&gpu_ext_off[at + 1], &gpu_ext_off[at], (gpu_next - at) * sizeof(uint64_t));
			memmove(&gpu_ext_len[at + 1], &gpu_ext_len[at], (gpu_next - at) * sizeof(uint64_t));
			gpu_next++;
		}
		gpu_ext_off[at] = best_at + bytes;
		gpu_ext_len[at] = end - (best_at + bytes);
	}
	if (!front && !back) {
		memmove(&gpu_ext_off[best], &gpu_ext_off[best + 1], (gpu_next - best - 1) * sizeof(uint64_t));
		memmove(&gpu_ext_len[best], &gpu_ext_len[best + 1], (gpu_next - best - 1) * sizeof(uint64_t));
		gpu_next--;
	}
	return best_at;
}

static void
gpu_pool_free(uint64_t off, uint64_t bytes)
{
	unsigned i = 0;
	while (i < gpu_next && gpu_ext_off[i] < off) {
		i++;
	}
	bool prev = i > 0 && gpu_ext_off[i - 1] + gpu_ext_len[i - 1] == off;
	bool next = i < gpu_next && off + bytes == gpu_ext_off[i];
	if (prev && next) {
		gpu_ext_len[i - 1] += bytes + gpu_ext_len[i];
		memmove(&gpu_ext_off[i], &gpu_ext_off[i + 1], (gpu_next - i - 1) * sizeof(uint64_t));
		memmove(&gpu_ext_len[i], &gpu_ext_len[i + 1], (gpu_next - i - 1) * sizeof(uint64_t));
		gpu_next--;
	} else if (prev) {
		gpu_ext_len[i - 1] += bytes;
	} else if (next) {
		gpu_ext_off[i] = off;
		gpu_ext_len[i] += bytes;
	} else if (gpu_next < GPU_POOL_EXTENTS) {
		memmove(&gpu_ext_off[i + 1], &gpu_ext_off[i], (gpu_next - i) * sizeof(uint64_t));
		memmove(&gpu_ext_len[i + 1], &gpu_ext_len[i], (gpu_next - i) * sizeof(uint64_t));
		gpu_ext_off[i] = off;
		gpu_ext_len[i] = bytes;
		gpu_next++;
	} else {
		printf("PANDORA GPU: pool extent list full: %#llx bytes at %#llx lost\n", bytes, off);
	}
}

/* For level 13: a buffer from the pool, with its physical address. */
static vm_offset_t
gpu_pool_take(uint64_t bytes, uint64_t *pa)
{
	uint64_t off = gpu_pool_alloc(bytes, 0x1000);
	if (off == ~0ull) {
		return 0;
	}
	*pa = gpu_pool_pa + off;
	return gpu_pool_map(off, bytes);        /* kept for the rest of the boot */
}

static uint32_t
pkt4(uint32_t reg, uint32_t cnt)
{
	return 0x40000000u | cnt | (pm4_parity(cnt) << 7) | ((reg & 0x3ffffu) << 8) | (pm4_parity(reg) << 27);
}

/* A8XX registers for the submission frame (gen8_reg.h; a6xx.xml CP_SCRATCH_GLOBAL at 0x82a). */
#define RBBM_PERFCTR_SRAM_INIT_CMD        0x449u
#define RBBM_SLICE_PERFCTR_SRAM_INIT_CMD  0x5e8u
#define RBBM_PERFCTR_SRAM_INIT_STATUS     0x19fu
#define CP_SCRATCH_GLOBAL2                0x82cu
/* msm_rbmemptrs in the scratch page: rptr +0, fence +4, bv_rptr +8, bv_fence +12; the CP's
 * read-pointer copies already sit at scratch + 0x100 and + 0x108 (level 10). */
#define MEMPTRS_OFF       0x100u

static uint32_t gpu_seqno;

/* The last submissions, by fence (0193, gpu_hang_dump). */
#define GPU_HIST 16u
struct gpu_hist {
	uint32_t seq, thread, n, first, dw0, dwn;
	int client;
	uint64_t ib0, ibn;
};
static struct gpu_hist gpu_hist[GPU_HIST];

static void
ring_put_n(uint32_t op, const uint32_t *p, uint32_t n)
{
	ring_put(pkt7(op, n));
	for (uint32_t i = 0; i < n; i++) {
		ring_put(p[i]);
	}
}

/* One submission as a7xx_submit (Linux) and NativeAdrenoRing frame it: BR, the context entry of
 * a6xx_set_pagetable on the first one, cache flush and invalidate, THREAD (BR 1 or BOTH 3) and
 * IFPC off, the IB, then the fence: scratch, CCU invalidates, BV timestamp, BR waits for it,
 * ring timestamp, BOTH, IFPC on. Returns the sequence number. */
static uint32_t
gpu_submit(const uint64_t *ib, const uint32_t *dwords, unsigned n, uint32_t thread, bool reset)
{
	const uint64_t mem = gpu_bufs[2].iova + MEMPTRS_OFF, fence = mem + 4, bv = mem + 12;
	const uint32_t seq = ++gpu_seqno;
	uint32_t v;
	v = 0x80000001u; ring_put_n(0x17, &v, 1);               /* CP_THREAD_CONTROL SYNC|BR */
	if (reset) {
		const uint32_t wait[4] = { 0, (uint32_t)fence, (uint32_t)(fence >> 32), seq - 1 };
		ring_put_n(0x14, wait, 4);                       /* CP_WAIT_TIMESTAMP */
		v = 0x80000003u; ring_put_n(0x17, &v, 1);
		v = 0xf; ring_put_n(0x1f, &v, 1);                /* CP_RESET_CONTEXT_STATE */
		v = 0x80000003u; ring_put_n(0x17, &v, 1);
		v = 0x26; ring_put_n(0x46, &v, 1);               /* LRZ_FLUSH_INVALIDATE */
		v = 0x80000001u; ring_put_n(0x17, &v, 1);
		ring_put(pkt4(RBBM_PERFCTR_SRAM_INIT_CMD, 1));
		ring_put(1);
		ring_put(pkt4(RBBM_SLICE_PERFCTR_SRAM_INIT_CMD, 1));
		ring_put(1);
		v = 0x31; ring_put_n(0x46, &v, 1);               /* CACHE_INVALIDATE */
		const uint32_t sram[6] = { 3, RBBM_PERFCTR_SRAM_INIT_STATUS, 0, 1, 1, 0 };
		ring_put_n(0x3c, sram, 6);                       /* CP_WAIT_REG_MEM, WRITE_EQ */
	}
	v = 0x32; ring_put_n(0x46, &v, 1);                       /* CACHE_FLUSH7 */
	v = 0x33; ring_put_n(0x46, &v, 1);                       /* CACHE_INVALIDATE7 */
	ring_put_n(0x26, NULL, 0);                               /* CP_WAIT_FOR_IDLE */
	ring_put_n(0x17, &thread, 1);
	v = 0x101; ring_put_n(0x65, &v, 1);                      /* CP_SET_MARKER: IFPC off */
	for (unsigned i = 0; i < n; i++) {
		const uint32_t ibp[3] = { (uint32_t)ib[i], (uint32_t)(ib[i] >> 32), dwords[i] };
		ring_put_n(0x3f, ibp, 3);                        /* CP_INDIRECT_BUFFER */
	}
	ring_put(pkt4(CP_SCRATCH_GLOBAL2, 1));
	ring_put(seq);
	v = 1; ring_put_n(0x17, &v, 1);
	v = 0x18; ring_put_n(0x46, &v, 1);                       /* CCU_INVALIDATE_DEPTH */
	v = 0x19; ring_put_n(0x46, &v, 1);                       /* CCU_INVALIDATE_COLOR */
	v = 2; ring_put_n(0x17, &v, 1);
	const uint32_t bvw[4] = { 0x31u | (1u << 27), (uint32_t)bv, (uint32_t)(bv >> 32), seq };
	ring_put_n(0x46, bvw, 4);                                /* BV timestamp */
	v = 1; ring_put_n(0x17, &v, 1);
	const uint32_t bvwait[4] = { 0, (uint32_t)bv, (uint32_t)(bv >> 32), seq };
	ring_put_n(0x14, bvwait, 4);
	const uint32_t fw[4] = { 0x31u | (1u << 27) | (1u << 31), (uint32_t)fence, (uint32_t)(fence >> 32), seq };
	ring_put_n(0x46, fw, 4);                                 /* ring timestamp, IRQ */
	v = 3; ring_put_n(0x17, &v, 1);
	v = 0x100; ring_put_n(0x65, &v, 1);                      /* IFPC on */
	ring_kick();
	return seq;
}

/* Until the ring timestamp reaches seq, at most ms; logs both timestamps and CP_SCRATCH_GLOBAL(2). */
static bool
gpu_wait_fence(uint32_t seq, unsigned ms)
{
	const volatile uint32_t *mem = (const volatile uint32_t *)(gpu_bufs[2].va + MEMPTRS_OFF);
	uint32_t f = 0;
	for (unsigned t = 0; t < ms * 10; t++) {
		flush_dcache64((addr64_t)gpu_bufs[2].pa + MEMPTRS_OFF, 16, TRUE);
		f = mem[1];
		if ((int32_t)(f - seq) >= 0) {
			break;
		}
		delay(100);
	}
	bool ok = (int32_t)(f - seq) >= 0;
	printf("PANDORA GPU: fence %u %s: ring timestamp %u, bv timestamp %u, scratch_global2 %u\n", seq,
	    ok ? "reached" : "NOT reached", f, mem[3], gr(CP_SCRATCH_GLOBAL2));
	(void)ring_idle(10);
	return ok;
}

#define GPU_TEST_IOVA     0x1f000000ull     /* below the user windows of level 14 */

static bool
gpu_level13(void)
{
	bool ok;
	if (!gpu_pool_bytes) {
		printf("PANDORA GPU: no buffer pool\n");
		return false;
	}
	/* A 64 KiB buffer: the IB at +0, its target at +0x8000, mapped at the first user address. */
	uint64_t bo_pa;
	vm_offset_t bo = gpu_pool_take(0x10000, &bo_pa);
	gpu_step("SMMU: map a pool buffer at 0x20000000");
	if (!bo || !smmu_map(&gpu_smmu, GPU_TEST_IOVA, bo_pa, 0x10000, ArmSmmu4KRead | ArmSmmu4KWrite)) {
		printf("PANDORA GPU: test buffer not mapped\n");
		return false;
	}
	printf("PANDORA GPU: test buffer pa %#llx at iova %#llx; table pages used %u of %u\n", bo_pa,
	    GPU_TEST_IOVA, gpu_smmu.tnext, gpu_smmu.tpages);

	gmu_wr(GMU_HOST2GMU_INTR_SET, 1u << 30);
	(void)gpu_poll(gpu_gmu, GMU_REG(GMU_GMU2HOST_INTR_INFO), 1u << 31, 1u << 31, 100000, &ok);
	gmu_wr(GMU_GMU2HOST_INTR_CLR, 1u << 31);
	if (!ok) {
		printf("PANDORA GPU: OOB GPU_SET NOT ACKED\n");
		return false;
	}
	/* Two submissions: the first with the context entry and the IB on BR, the second with the
	 * IB on BOTH as a7xx_submit runs user IBs. Each IB writes its own word. */
	static const uint32_t threads[2] = { 1, 3 };
	bool all = true;
	for (unsigned k = 0; k < 2 && all; k++) {
		uint32_t *ib = (uint32_t *)(bo + 0x100 * k);
		const uint64_t target = GPU_TEST_IOVA + 0x8000 + 4 * k;
		ib[0] = pkt7(0x3d, 3);          /* CP_MEM_WRITE */
		ib[1] = (uint32_t)target;
		ib[2] = (uint32_t)(target >> 32);
		ib[3] = 0x13130000u + k;
		gpu_pool_sync(bo_pa - gpu_pool_pa + 0x100 * k, 16, false);
		char what[64];
		snprintf(what, sizeof(what), "CP: framed submission %u (IB on %s)", k + 1, k ? "BOTH" : "BR");
		gpu_step(what);
		const uint64_t ibva = GPU_TEST_IOVA + 0x100 * k;
		const uint32_t ibdw = 4;
		uint32_t seq = gpu_submit(&ibva, &ibdw, 1, threads[k], k == 0);
		bool f = gpu_wait_fence(seq, 1000);
		gpu_pool_sync(bo_pa - gpu_pool_pa + 0x8000, 64, true);
		uint32_t w = ((const volatile uint32_t *)(bo + 0x8000))[k];
		printf("PANDORA GPU: submission %u: buffer word %u = %08x (want %08x)\n", seq, k, w, 0x13130000u + k);
		all = f && w == 0x13130000u + k;
	}
	gmu_wr(GMU_HOST2GMU_INTR_SET, 1u << 31);
	return all;
}

/* ---- level 14: buffer objects and submissions for the user client ------------------------------ */

/*
 * What iokit/Kernel/arm/PandoraAdrenoGpu.cpp (NativeAdreno's user ABI) stands on: buffers from the
 * pool with handles, GPU mappings at addresses the client picks inside its window, CPU views
 * by token, framed submissions and fence waits. One lock for everything, as NativeAdreno's
 * global lock; OOB GPU_SET is held from level 14 on, so the GPU never powers down under us.
 * Windows as NativeAdrenoPublish lays them out: 1 GiB at 0x20000000, then three of 0xaaa9000.
 */
LCK_GRP_DECLARE(gpu_lck_grp, "pandora_gpu");
LCK_MTX_DECLARE(gpu_lock, &gpu_lck_grp);

#define GPU_MAX_BOS       4096u
#define GPU_MAX_CLIENTS   4u
#define GPU_MAX_IBS       128u      /* NativeAdrenoAbiMaxBuffers (tools/turnip/abi_128.py, 0194) */
struct gpu_bo {
	bool used;
	int client;
	uint32_t gen, perm, leases;
	uint64_t off, bytes, iova;      /* iova 0 = not mapped */
	uint64_t *pages;                /* an imported buffer's 4K pages (BoImport, patch 0187), else NULL */
	void *ref;                      /* its pinning descriptor, given back by pandora_gpu_release_ref */
	bool readonly;                  /* imported pages the caller may only read: the GPU maps them read-only */
	uint32_t meta_len;
	char meta[64];
};
static struct gpu_bo gpu_bos[GPU_MAX_BOS];

/* iokit/Kernel/arm/PandoraAdrenoGpu.cpp: unpin an imported buffer's pages. */
extern void pandora_gpu_release_ref(void *ref);

/* Under the lock: unmap and give back a buffer, from the pool or imported. */
static void
gpu_bo_release(struct gpu_bo *b)
{
	if (b->iova) {
		smmu_unmap(&gpu_smmu, b->iova, b->bytes);
	}
	if (b->pages) {
		kfree_data(b->pages, (vm_size_t)(b->bytes >> 12) * sizeof(uint64_t));
		pandora_gpu_release_ref(b->ref);
	} else {
		gpu_pool_free(b->off, b->bytes);
	}
	b->pages = NULL;
	b->ref = NULL;
	b->used = false;
}
static struct {
	bool used, reset;
	uint64_t va_start, va_size;
	char name[24];                  /* the opening process (patch 0189: the watch log) */
	uint32_t allocs, imports, maps, submits, frees, fence;
} gpu_clients[GPU_MAX_CLIENTS];
static const uint64_t gpu_window[GPU_MAX_CLIENTS][2] = {
	{ 0x20000000ull, 0x40000000ull }, { 0x60000000ull, 0xaaa9000ull },
	{ 0x6aaa9000ull, 0xaaa9000ull }, { 0x75552000ull, 0xaaa9000ull },
};
static bool gpu_ready;
static uint32_t gpu_bo_gen;
static uint32_t gpu_chip_id = 0x44050a01u;

uint32_t pandora_gpu_chip_id(void);
uint32_t
pandora_gpu_chip_id(void)
{
	return gpu_chip_id;
}

/* Clear the leaves of [iova, iova + bytes) and TLBIALL; table pages stay. */
static void
smmu_unmap(struct smmu_ctx *c, uint64_t iova, uint64_t bytes)
{
	uint64_t pool = c->tables.root_physical;
	unsigned in = c->tcfg.input_bits, start = c->tables.start_level;
	for (uint64_t off = 0; off < bytes; off += 0x1000) {
		uint64_t va = iova + off, *t = (uint64_t *)c->tva;
		for (unsigned level = start; level < 3 && t; level++) {
			unsigned shift = 39 - 9 * level, width = level == start ? in - shift : 9;
			uint64_t e = t[(va >> shift) & ((1u << width) - 1)];
			t = e ? (uint64_t *)(c->tva + ((e & 0xfffffffff000ull) - pool)) : NULL;
		}
		if (t) {
			uint64_t *leaf = &t[(va >> 12) & 511];
			*leaf = 0;
			clean_dcache64((addr64_t)(pool + ((vm_offset_t)leaf - c->tva)), 8, TRUE);
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	ArmSmmu4KProgramResult r;
	(void)ArmSmmu4KInvalidateAllOwnedContext(&smmu_io, c->bank, &c->ctx, 1000, &r);
}

static struct gpu_bo *
gpu_bo_get(int client, uint64_t handle)
{
	uint32_t idx = (uint32_t)handle - 1, gen = (uint32_t)(handle >> 32);
	if (idx >= GPU_MAX_BOS || !gpu_bos[idx].used || gpu_bos[idx].client != client || gpu_bos[idx].gen != gen) {
		return NULL;
	}
	return &gpu_bos[idx];
}

static bool
gpu_fence_done(uint32_t seq)
{
	const volatile uint32_t *mem = (const volatile uint32_t *)(gpu_bufs[2].va + MEMPTRS_OFF);
	flush_dcache64((addr64_t)gpu_bufs[2].pa + MEMPTRS_OFF, 16, TRUE);
	return (int32_t)(mem[1] - seq) >= 0;
}

/* The last fence issued, 0 while the GPU is not up (iokit/Kernel/arm/SweetFramebuffer.cpp, 0192). */
uint32_t pandora_gpu_issued(void);
uint32_t
pandora_gpu_issued(void)
{
	return gpu_ready ? gpu_seqno : 0;
}

/* Without the lock: until fence seq or the timeout (0 = look once). 0, ETIMEDOUT, EINVAL. */
int pandora_gpu_wait(uint64_t fence, uint64_t timeout_ns);
int
pandora_gpu_wait(uint64_t fence, uint64_t timeout_ns)
{
	uint32_t seq = (uint32_t)fence;
	if (fence >> 32 || (int32_t)(seq - gpu_seqno) > 0) {
		return EINVAL;
	}
	uint64_t t0 = mach_absolute_time(), ns = 0;
	while (!gpu_fence_done(seq)) {
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		if (ns >= timeout_ns) {
			return ETIMEDOUT;
		}
		delay(100);
	}
	return 0;
}

/* A client and its window; -1 while the GPU is not up or all windows are taken. */
int pandora_gpu_open(void);
int
pandora_gpu_open(void)
{
	int c = -1;
	lck_mtx_lock(&gpu_lock);
	for (unsigned i = 0; gpu_ready && i < GPU_MAX_CLIENTS && c < 0; i++) {
		if (!gpu_clients[i].used) {
			gpu_clients[i].used = true;
			gpu_clients[i].reset = true;
			gpu_clients[i].va_start = gpu_window[i][0];
			gpu_clients[i].va_size = gpu_window[i][1];
			gpu_clients[i].allocs = gpu_clients[i].imports = gpu_clients[i].maps = 0;
			gpu_clients[i].submits = gpu_clients[i].frees = gpu_clients[i].fence = 0;
			proc_selfname(gpu_clients[i].name, sizeof(gpu_clients[i].name));
			c = (int)i;
		}
	}
	lck_mtx_unlock(&gpu_lock);
	return c;
}

void pandora_gpu_window(int client, uint64_t *start, uint64_t *size);
void
pandora_gpu_window(int client, uint64_t *start, uint64_t *size)
{
	*start = gpu_clients[client].va_start;
	*size = gpu_clients[client].va_size;
}

/* After the client's work (at most 2 s, as NativeAdrenoPublish waits), every buffer it owns goes. */
void pandora_gpu_close(int client);
void
pandora_gpu_close(int client)
{
	(void)pandora_gpu_wait(gpu_seqno, 2000000000ull);
	lck_mtx_lock(&gpu_lock);
	unsigned n = 0;
	for (unsigned i = 0; i < GPU_MAX_BOS; i++) {
		struct gpu_bo *b = &gpu_bos[i];
		if (b->used && b->client == client) {
			gpu_bo_release(b);
			n++;
		}
	}
	gpu_clients[client].used = false;
	lck_mtx_unlock(&gpu_lock);
	printf("PANDORA GPU: client %d closed, %u buffers freed\n", client, n);
}

int pandora_gpu_bo_alloc(int client, uint64_t bytes, uint32_t align_log2, uint64_t *handle);
int
pandora_gpu_bo_alloc(int client, uint64_t bytes, uint32_t align_log2, uint64_t *handle)
{
	if (!bytes || (bytes & 0xfff) || bytes > (256ull << 20) || align_log2 < 12 || align_log2 > 24) {
		return EINVAL;
	}
	int err = ENOMEM;
	lck_mtx_lock(&gpu_lock);
	for (unsigned i = 0; i < GPU_MAX_BOS; i++) {
		struct gpu_bo *b = &gpu_bos[i];
		if (b->used) {
			continue;
		}
		uint64_t off = gpu_pool_alloc(bytes, 1ull << align_log2);
		if (off != ~0ull) {
			/* Zeroed, and out of the CPU caches when the kernel's view is cacheable: the
			 * client maps it write-combined and the GPU reads it non-cacheably. */
			gpu_pool_zero(off, bytes);
			*b = (struct gpu_bo){ .used = true, .client = client, .gen = ++gpu_bo_gen, .off = off, .bytes = bytes };
			*handle = ((uint64_t)b->gen << 32) | (i + 1);
			err = 0;
			gpu_clients[client].allocs++;
		}
		break;
	}
	lck_mtx_unlock(&gpu_lock);
	return err;
}

/* PERM bit 0 read (required), bit 1 write; the address must lie inside the client's window. */
int pandora_gpu_bo_map(int client, uint64_t handle, uint64_t iova, uint32_t perm);
int
pandora_gpu_bo_map(int client, uint64_t handle, uint64_t iova, uint32_t perm)
{
	int err = 0;
	lck_mtx_lock(&gpu_lock);
	struct gpu_bo *b = gpu_bo_get(client, handle);
	uint64_t ws = gpu_clients[client].va_start, we = ws + gpu_clients[client].va_size;
	if (!b || (iova & 0xfff) || !(perm & 1) || (perm & ~3u) || iova < ws || iova + b->bytes > we) {
		err = EINVAL;
	} else if (b->iova) {
		err = EBUSY;
	} else if (!smmu_map_pages(&gpu_smmu, iova, gpu_pool_pa + b->off, b->pages, b->bytes,
	    ArmSmmu4KRead | ((perm & 2) && !b->readonly ? ArmSmmu4KWrite : 0))) {
		err = EEXIST;
	} else {
		b->iova = iova;
		b->perm = perm;
		gpu_clients[client].maps++;
	}
	lck_mtx_unlock(&gpu_lock);
	return err;
}

/* Refused while CPU views remain; waits for every submission so far, then unmaps and frees. */
int pandora_gpu_bo_free(int client, uint64_t handle);
int
pandora_gpu_bo_free(int client, uint64_t handle)
{
	if (pandora_gpu_wait(gpu_seqno, 2000000000ull)) {
		return EBUSY;
	}
	int err = 0;
	lck_mtx_lock(&gpu_lock);
	struct gpu_bo *b = gpu_bo_get(client, handle);
	if (!b) {
		err = EINVAL;
	} else if (b->leases) {
		err = EBUSY;
	} else {
		gpu_bo_release(b);
		gpu_clients[client].frees++;
	}
	lck_mtx_unlock(&gpu_lock);
	return err;
}

/* CPU views: DELTA +1 takes a lease and returns the token (entry + 1), -1 gives one back; the
 * token's physical range for clientMemoryForType. */
int pandora_gpu_bo_lease(int client, uint64_t handle, int delta, uint32_t *token, uint64_t *bytes,
    uint32_t *leases);
int
pandora_gpu_bo_lease(int client, uint64_t handle, int delta, uint32_t *token, uint64_t *bytes,
    uint32_t *leases)
{
	int err = 0;
	lck_mtx_lock(&gpu_lock);
	struct gpu_bo *b = gpu_bo_get(client, handle);
	if (!b || b->pages || (delta < 0 && !b->leases)) {     /* an imported buffer has no CPU view token */
		err = EINVAL;
	} else {
		b->leases += delta;
		*token = (uint32_t)(b - gpu_bos) + 1;
		*bytes = b->bytes;
		*leases = b->leases;
	}
	lck_mtx_unlock(&gpu_lock);
	return err;
}

/*
 * BoImport: a buffer over the caller's own pinned pages (an IOSurface's), PAGES[N] physical, REF
 * the descriptor that pins them. It keeps the caller's CPU mapping and cache mode; the GPU maps
 * it non-cacheable, and every submission of the client cleans and invalidates those pages first.
 * READONLY: the caller's mapping is read-only (bb9: a surface backboardd only reads); the GPU
 * mapping then has no write permission whatever BoMap asks.
 */
int pandora_gpu_bo_import(int client, const uint64_t *pages, uint32_t n, void *ref, bool readonly,
    uint64_t *handle);
int
pandora_gpu_bo_import(int client, const uint64_t *pages, uint32_t n, void *ref, bool readonly,
    uint64_t *handle)
{
	if (!n || n > (256u << 20) >> 12) {
		return EINVAL;
	}
	uint64_t *copy = kalloc_data(n * sizeof(uint64_t), Z_WAITOK);
	if (!copy) {
		return ENOMEM;
	}
	memcpy(copy, pages, n * sizeof(uint64_t));
	int err = ENOMEM;
	lck_mtx_lock(&gpu_lock);
	for (unsigned i = 0; i < GPU_MAX_BOS; i++) {
		struct gpu_bo *b = &gpu_bos[i];
		if (!b->used) {
			*b = (struct gpu_bo){ .used = true, .client = client, .gen = ++gpu_bo_gen,
				              .bytes = (uint64_t)n << 12, .pages = copy, .ref = ref,
				              .readonly = readonly };
			*handle = ((uint64_t)b->gen << 32) | (i + 1);
			err = 0;
			gpu_clients[client].imports++;
			break;
		}
	}
	lck_mtx_unlock(&gpu_lock);
	if (err) {
		kfree_data(copy, n * sizeof(uint64_t));
	}
	return err;
}

int pandora_gpu_token_phys(int client, uint32_t token, uint64_t *pa, uint64_t *bytes);
int
pandora_gpu_token_phys(int client, uint32_t token, uint64_t *pa, uint64_t *bytes)
{
	int err = EINVAL;
	lck_mtx_lock(&gpu_lock);
	if (token >= 1 && token <= GPU_MAX_BOS) {
		struct gpu_bo *b = &gpu_bos[token - 1];
		if (b->used && b->client == client && b->leases) {
			*pa = gpu_pool_pa + b->off;
			*bytes = b->bytes;
			err = 0;
		}
	}
	lck_mtx_unlock(&gpu_lock);
	return err;
}

int pandora_gpu_bo_meta(int client, uint64_t handle, bool set, char *text, uint32_t *len);
int
pandora_gpu_bo_meta(int client, uint64_t handle, bool set, char *text, uint32_t *len)
{
	int err = 0;
	lck_mtx_lock(&gpu_lock);
	struct gpu_bo *b = gpu_bo_get(client, handle);
	if (!b || (set && *len > sizeof(b->meta))) {
		err = EINVAL;
	} else if (set) {
		memcpy(b->meta, text, *len);
		b->meta_len = *len;
	} else {
		memcpy(text, b->meta, b->meta_len);
		*len = b->meta_len;
	}
	lck_mtx_unlock(&gpu_lock);
	return err;
}

/* Each IB inside one of the client's readable mapped buffers, at most 0xfffff dwords. The IBs
 * run on BOTH when the first one starts with its own CP_THREAD_CONTROL (Turnip's tu_init_hw,
 * 0x70970001), else on BR: the myron driver found the A840 hangs with all jobs on BR and with
 * continuation jobs on BOTH. */
/* 0195: boot-arg pandora_gpu_flushall=1 brings back the flush of every imported page per submission. */
static bool
gpu_flush_all(void)
{
	static int on = -1;
	if (on < 0) {
		on = 0;
		(void)PE_parse_boot_argn("pandora_gpu_flushall", &on, sizeof(on));
	}
	return on != 0;
}

/* 0195: time in pandora_gpu_submit (lookups, ring room, flush, framing), a line per 500 submissions. */
static uint64_t gpu_stat_submits, gpu_stat_ns, gpu_stat_max_ns, gpu_stat_flushed, gpu_stat_ibs;

/* 0198: the frequency governor's wait channel; a submission after 50 ms without one sets the kick and
 * wakes it (gpu_dcvs_thread boosts, as msm_devfreq_active does). */
static int gpu_dcvs_chan;
static uint32_t gpu_dcvs_kick;
static uint64_t gpu_last_submit_abs;

int pandora_gpu_submit(int client, const uint64_t *iova, const uint64_t *bytes, unsigned n, uint64_t *fence);
int
pandora_gpu_submit(int client, const uint64_t *iova, const uint64_t *bytes, unsigned n, uint64_t *fence)
{
	if (!n || n > GPU_MAX_IBS) {
		return EINVAL;
	}
	const uint64_t t0 = mach_absolute_time();
	/* Off the kernel stack: 128 IBs (0194). */
	uint64_t *ib = kalloc_data(n * sizeof(uint64_t), Z_WAITOK);
	uint32_t *dw = kalloc_data(n * sizeof(uint32_t), Z_WAITOK), first = 0;
	if (!ib || !dw) {
		kfree_data(ib, n * sizeof(uint64_t));
		kfree_data(dw, n * sizeof(uint32_t));
		return ENOMEM;
	}
	int err = 0;
	lck_mtx_lock(&gpu_lock);
	for (unsigned i = 0; i < n && !err; i++) {
		err = EINVAL;
		if ((iova[i] & 3) || !bytes[i] || (bytes[i] & 3) || bytes[i] / 4 > 0xfffff) {
			break;
		}
		for (unsigned j = 0; j < GPU_MAX_BOS; j++) {
			struct gpu_bo *b = &gpu_bos[j];
			if (b->used && b->client == client && b->iova && (b->perm & 1) && iova[i] >= b->iova &&
			    iova[i] + bytes[i] <= b->iova + b->bytes) {
				if (i == 0 && !b->pages) {
					uint64_t pa = gpu_pool_pa + b->off + (iova[i] - b->iova);
					gpu_pool_sync(pa - gpu_pool_pa, 4, true);
					vm_offset_t fv = gpu_pool_map(pa - gpu_pool_pa, 4);
					first = fv ? *(const volatile uint32_t *)fv : 0;
					gpu_pool_unmap(fv, pa - gpu_pool_pa, 4);
				}
				ib[i] = iova[i];
				dw[i] = (uint32_t)(bytes[i] / 4);
				err = 0;
				break;
			}
		}
	}
	/* Room in the ring for the frame (at most ~110 dwords) and the IB packets. */
	const uint32_t size = (uint32_t)(gpu_bufs[0].bytes / 4), need = 128 + 4 * n;
	for (unsigned t = 0; !err; t++) {
		uint32_t rptr = gr(CP_RB_RPTR);
		if (((rptr - ring_wptr - 1) & (size - 1)) >= need) {
			break;
		}
		if (t == 10000) {
			err = EBUSY;
		}
		delay(100);
	}
	if (!err && gpu_flush_all()) {
		/* The client's imported buffers out of the CPU caches: their pages are the caller's
		 * cacheable memory, the GPU reads and writes them non-cacheably. 0195: only with
		 * pandora_gpu_flushall=1. NativeMetal already cleans and invalidates the surfaces each
		 * command buffer uses at commit and at completion (sys_dcache_flush, flushSurfaces); this
		 * loop went over every surface backboardd ever imported on every submission (bb21: 83
		 * surfaces, 27 ms from commit to completion for a 98-draw frame). */
		for (unsigned j = 0; j < GPU_MAX_BOS; j++) {
			struct gpu_bo *b = &gpu_bos[j];
			if (b->used && b->client == client && b->pages && b->iova) {
				for (uint64_t k = 0; k < b->bytes >> 12; k++) {
					flush_dcache64((addr64_t)b->pages[k], 0x1000, TRUE);
				}
				gpu_stat_flushed += b->bytes;
			}
		}
	}
	if (!err) {
		uint32_t thread = first == 0x70970001u ? 3 : 1;
		*fence = gpu_submit(ib, dw, n, thread, gpu_clients[client].reset);
		struct gpu_hist *h = &gpu_hist[(uint32_t)*fence % GPU_HIST];
		*h = (struct gpu_hist){ .seq = (uint32_t)*fence, .client = client, .thread = thread, .n = n,
			                .first = first, .ib0 = ib[0], .dw0 = dw[0], .ibn = ib[n - 1], .dwn = dw[n - 1] };
		gpu_clients[client].submits++;
		gpu_clients[client].fence = (uint32_t)*fence;
		gpu_clients[client].reset = false;
		uint64_t gap;
		absolutetime_to_nanoseconds(t0 - gpu_last_submit_abs, &gap);
		if (gpu_last_submit_abs && gap >= 50ull * NSEC_PER_MSEC) {
			__atomic_store_n(&gpu_dcvs_kick, 1, __ATOMIC_RELAXED);
			thread_wakeup(&gpu_dcvs_chan);
		}
		gpu_last_submit_abs = t0;
		uint64_t ns;
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		gpu_stat_ns += ns;
		gpu_stat_max_ns = MAX(gpu_stat_max_ns, ns);
		gpu_stat_ibs += n;
		if (++gpu_stat_submits % 500 == 0) {
			printf("PANDORA GPU SUBMIT: %llu submissions, avg %llu us, max %llu us, %llu IBs each, "
			    "flushed %llu MiB each (flushall %d)\n", gpu_stat_submits, gpu_stat_ns / 500 / 1000,
			    gpu_stat_max_ns / 1000, gpu_stat_ibs / 500, gpu_stat_flushed / 500 >> 20, gpu_flush_all());
			gpu_stat_ns = gpu_stat_max_ns = gpu_stat_flushed = gpu_stat_ibs = 0;
		}
	}
	lck_mtx_unlock(&gpu_lock);
	kfree_data(ib, n * sizeof(uint64_t));
	kfree_data(dw, n * sizeof(uint32_t));
	return err;
}

/* CP_ALWAYS_ON_COUNTER (A8XX 0x8e7, as the myron chip table has it). */
uint64_t pandora_gpu_timestamp(void);
uint64_t
pandora_gpu_timestamp(void)
{
	lck_mtx_lock(&gpu_lock);
	uint32_t hi = gr(0x8e8), lo = gr(0x8e7), hi2 = gr(0x8e8);
	if (hi2 != hi) {
		lo = gr(0x8e7);
	}
	lck_mtx_unlock(&gpu_lock);
	return ((uint64_t)hi2 << 32) | lo;
}

/* ---- level 15: the rest of a8xx hw_init, the state rendering needs ---------------------------- */

/*
 * Linux a8xx_gpu.c hw_init after what levels 10-11 already do, in its order: GPU busy counter
 * mask, GMEM range (64 MiB), UCHE trap and write-through bases, perf counters, UBWC
 * (a8xx_set_ubwc_config for kaanapali: UBWC 6.0, highest bank bit 16, swizzle levels 2+3),
 * bicubic weights, UCHE_CLIENT_PF, the A840 non-context list (a6xx_catalog.c a840_nonctxt_regs),
 * interface hang detection, CP register protection (a840_protect_regs), GMEM save/restore,
 * per-pipe CP interrupt and fault masks, the global masks, GMEM_PROTECT (after the zap, as
 * Linux), and hardware clock gating left off (GBIF_CX_CONFIG bit 0 cleared, Linux's
 * a8xx_set_hwcg(false)). Not done: the GMU power counters and the IFPC power-up list (no IFPC).
 * Offsets from a6xx.xml (domain A6XX, A8XX variants), each checked against gen8_reg.h.
 */
#define CP_APERTURE_PIPE(p)   ((uint32_t)(p) << 12)    /* PIPEID 12..15, slice 0 */
#define PIPE_NONE 0u
#define PIPE_BR   1u
#define PIPE_BV   2u

struct gpu_pipe_reg {
	uint32_t off, val, pipes;       /* pipes: BIT(PIPE_NONE) | BIT(PIPE_BR) | BIT(PIPE_BV) */
};

static const struct gpu_pipe_reg a840_nonctxt[] = {
	{ 0x00814, 0x00000101, 0x1 },   /* CP_SMMU_STREAM_ID_LPAC */
	{ 0x08702, 0x00000800, 0x6 },   /* GRAS_DBG_ECO_CNTL */
	{ 0x08600, 0x00200000, 0x6 },   /* GRAS_TSEFE_DBG_ECO_CNTL */
	{ 0x09e0a, 0x00000001, 0x6 },   /* PC_AUTO_VERTEX_STRIDE */
	{ 0x09e0d, 0x10010000, 0x6 },   /* PC_VIS_STREAM_CNTL */
	{ 0x09e64, 0x00000002, 0x6 },   /* PC_CONTEXT_SWITCH_STABILIZE_CNTL_1 */
	{ 0x09e50, 0x00000003, 0x6 },   /* PC_CHICKEN_BITS_1 */
	{ 0x09f20, 0x00000200, 0x6 },   /* PC_CHICKEN_BITS_2 */
	{ 0x09e22, 0x00500000, 0x6 },   /* PC_CHICKEN_BITS_3 */
	{ 0x09e23, 0x00500050, 0x6 },   /* PC_CHICKEN_BITS_4 */
	{ 0x08e01, 0x00000020, 0x6 },   /* RB_RBP_CNTL: dead draw merge off */
	{ 0x08e07, 0x00000068, 0x2 },   /* RB_CCU_CNTL */
	{ 0x08e06, 0x00002200, 0x2 },   /* RB_CCU_DBG_ECO_CNTL */
	{ 0x08e09, 0x12000000, 0x2 },   /* RB_GC_GMEM_PROTECT */
	{ 0x08f01, 0x00000007, 0x2 },   /* RB_RESOLVE_PREFETCH_CNTL */
	{ 0x08f02, 0x00004000, 0x2 },   /* RB_CMP_DBG_ECO_CNTL */
	{ 0x00440, 0x00000001, 0x1 },   /* RBBM_NC_MODE_CNTL */
	{ 0x005ec, 0x00000001, 0x1 },   /* RBBM_SLICE_NC_MODE_CNTL */
	{ 0x0007d, 0x70809060, 0x1 },   /* RBBM_POWER_UP_RESET_SW_OVERRIDE */
	{ 0x0007e, 0x30000000, 0x1 },   /* RBBM_POWER_UP_RESET_SW_BV_OVERRIDE */
	{ 0x00010, 0x00000030, 0x1 },   /* RBBM_WAIT_IDLE_CLOCKS_CNTL */
	{ 0x00011, 0x00000030, 0x1 },   /* RBBM_WAIT_IDLE_CLOCKS_CNTL2 */
	{ 0x0002f, 0x0fffffff, 0x1 },   /* RBBM_INTERFACE_HANG_INT_CNTL */
	{ 0x00008, 0x22122212, 0x1 },   /* RBBM_GBIF_CLIENT_QOS_CNTL */
	{ 0x0009d, 0x00000040, 0x1 },   /* RBBM_CGC_P2S_CNTL */
	{ 0x0ae03, 0x05000000, 0x1 },   /* SP_CHICKEN_BITS */
	{ 0x0ae05, 0x00000008, 0x1 },   /* SP_SS_CHICKEN_BITS_0 */
	{ 0x0ae09, 0x00c21800, 0x1 },   /* SP_CHICKEN_BITS_2 */
	{ 0x0ae0a, 0x00300000, 0x1 },   /* SP_CHICKEN_BITS_3 */
	{ 0x0ae0b, 0x00000002, 0x1 },   /* SP_CHICKEN_BITS_4 */
	{ 0x0ae0f, 0x0000003f, 0x1 },   /* SP_PERFCTR_SHADER_MASK */
	{ 0x0ae6c, 0x00004000, 0x1 },   /* SP_HLSQ_DBG_ECO_CNTL */
	{ 0x0ae73, 0x00020000, 0x1 },   /* SP_HLSQ_DBG_ECO_CNTL_1 */
	{ 0x0ae74, 0x01000000, 0x1 },   /* SP_HLSQ_DBG_ECO_CNTL_2 */
	{ 0x0ae76, 0x00000080, 0x1 },   /* SP_HLSQ_DBG_ECO_CNTL_3 */
	{ 0x0ae6b, 0x00000080, 0x1 },   /* SP_HLSQ_TIMEOUT_THRESHOLD_DP */
	{ 0x0ae6d, 0x0001ff00, 0x1 },   /* SP_READ_SEL */
	{ 0x0b600, 0x10100000, 0x1 },   /* TPL1_DBG_ECO_CNTL */
	{ 0x0b602, 0x04000720, 0x1 },   /* TPL1_DBG_ECO_CNTL1 */
	{ 0x00e01, 0x80080000, 0x1 },   /* UCHE_MODE_CNTL */
	{ 0x00f01, 0x00001000, 0x1 },   /* UCHE_CCHE_MODE_CNTL */
	{ 0x00f02, 0x00000800, 0x1 },   /* UCHE_CCHE_CACHE_WAYS */
	{ 0x00e12, 0x010240e0, 0x1 },   /* UCHE_GBIF_GX_CONFIG */
	{ 0x00e10, 0x00000020, 0x1 },   /* UCHE_VARB_IDLE_TIMEOUT */
	{ 0x0a600, 0x00008000, 0x6 },   /* VFD_DBG_ECO_CNTL */
	{ 0x0a639, 0x00500050, 0x6 },   /* VFD_CB_BV_THRESHOLD */
	{ 0x0a63a, 0x00600060, 0x6 },   /* VFD_CB_BR_THRESHOLD */
	{ 0x0a63b, 0x00200020, 0x6 },   /* VFD_CB_BUSY_REQ_CNT */
	{ 0x0a63c, 0x00000020, 0x6 },   /* VFD_CB_LP_REQ_CNT */
	{ 0x09741, 0x00000001, 0x6 },   /* VPC_FLATSHADE_MODE_CNTL */
};

/* A6XX_PROTECT_RDONLY / _NORDWR (a6xx_gpu.h): length - 1 in bits 18..31, register in 0..17. */
#define PROT_RO(r, l)  ((((uint32_t)(l) & 0x3fff) << 18) | ((uint32_t)(r) & 0x3ffff))
#define PROT_NA(r, l)  ((1u << 31) | PROT_RO(r, l))
static const uint32_t a840_protect[] = {
	PROT_RO(0x00008, 0x039b), PROT_RO(0x003b4, 0x008b), PROT_NA(0x00440, 0x001f),
	PROT_RO(0x00580, 0x005f), PROT_NA(0x005e0, 0x011f), PROT_RO(0x0074a, 0x0005),
	PROT_RO(0x00759, 0x001b), PROT_NA(0x00775, 0x000a), PROT_RO(0x00789, 0x0000),
	PROT_RO(0x0078c, 0x0013), PROT_NA(0x00800, 0x0029), PROT_NA(0x00837, 0x00af),
	PROT_RO(0x008e7, 0x00c9), PROT_NA(0x008ec, 0x00c3), PROT_NA(0x009b1, 0x0250),
	PROT_NA(0x00c07, 0x0008), PROT_RO(0x00ce0, 0x0001), PROT_RO(0x00df0, 0x0000),
	PROT_NA(0x00df1, 0x0000), PROT_NA(0x00e01, 0x0000), PROT_NA(0x00e03, 0x1fff),
	PROT_NA(0x03c00, 0x00c5), PROT_RO(0x03cc6, 0x0039), PROT_NA(0x03d00, 0x1fff),
	PROT_NA(0x08600, 0x01ff), PROT_NA(0x08e00, 0x00ff), PROT_RO(0x08f00, 0x0000),
	PROT_NA(0x08f01, 0x01be), PROT_NA(0x09600, 0x01ff), PROT_RO(0x0981a, 0x02e5),
	PROT_NA(0x09e00, 0x01ff), PROT_NA(0x0a600, 0x01ff), PROT_NA(0x0a82e, 0x0000),
	PROT_NA(0x0ae00, 0x0000), PROT_NA(0x0ae02, 0x0004), PROT_NA(0x0ae08, 0x0006),
	PROT_NA(0x0ae10, 0x00bf), PROT_RO(0x0aed0, 0x002f), PROT_NA(0x0af00, 0x027f),
	PROT_NA(0x0b600, 0x1fff), PROT_NA(0x0dc00, 0x1fff), PROT_RO(0x0fc00, 0x1fff),
	PROT_NA(0x18400, 0x003f), PROT_RO(0x18440, 0x013f), PROT_NA(0x18580, 0x1fff),
	PROT_NA(0x1b400, 0x1fff), PROT_NA(0x1f400, 0x0477), PROT_RO(0x1f878, 0x0507),
	PROT_NA(0x1f930, 0x0329), PROT_NA(0x1fd80, 0x1fff), PROT_NA(0x27800, 0x007f),
	PROT_RO(0x27880, 0x0385), PROT_NA(0x27882, 0x0009), PROT_NA(0x27c06, 0x0000),
};

static const uint32_t a8xx_bicubic[20] = {
	0x00000000, 0x3fe05ff4, 0x3fa0ebee, 0x3f5193ed, 0x3f0243f0, 0x00000000, 0x3fd093e8, 0x3f4133dc,
	0x3ea1dfdb, 0x3e0283e0, 0x0000ac2b, 0x0000f01d, 0x00114412, 0x0021980a, 0x0051ec05, 0x0000380e,
	0x3ff09001, 0x3fc10bfa, 0x3f9193f7, 0x3f7227f7,
};

static void
gw64(uint32_t dw, uint64_t v)
{
	gw(dw, (uint32_t)v);
	gw(dw + 1, (uint32_t)(v >> 32));
}

static void
gw_pipe(uint32_t pipe, uint32_t dw, uint32_t v)
{
	gw(CP_APERTURE_CNTL_HOST, CP_APERTURE_PIPE(pipe));
	gw(dw, v);
}

static uint32_t
gr_pipe(uint32_t pipe, uint32_t dw)
{
	gw(CP_APERTURE_CNTL_HOST, CP_APERTURE_PIPE(pipe));
	return gr(dw);
}

/* Under OOB GPU_SET, CP idle, out of secure mode (levels 10-11). */
static void
gpu_level15(void)
{
	const uint64_t gmem_min = 64ull << 20, trap = 0x1fffffffff000ull;
	gpu_step("hw_init rest: GMEM range, UCHE trap, UBWC, non-context list, protection, masks");
	gw(CP_APERTURE_CNTL_HOST, 0);
	gw(0x19e, 0xffffffff);                  /* RBBM_PERFCTR_GPU_BUSY_MASKED */
	gw64(0xf08, gmem_min);                  /* UCHE_CCHE_GC_GMEM_RANGE_MIN */
	gw64(0xae10, gmem_min);                 /* SP_HLSQ_GC_GMEM_RANGE_MIN */
	gw64(0xe08, trap);                      /* UCHE_TRAP_BASE */
	gw64(0xe06, trap);                      /* UCHE_WRITE_THRU_BASE */
	gw64(0xf06, trap);                      /* UCHE_CCHE_TRAP_BASE */
	gw64(0xf04, trap);                      /* UCHE_CCHE_WRITE_THRU_BASE */
	gw(0x460, 1);                           /* RBBM_PERFCTR_CNTL */
	gw(0x500, 1);                           /* RBBM_SLICE_PERFCTR_CNTL */
	gw(0x8d0, 1);                           /* CP_PERFCTR_CP_SEL(0): CP0 counts cycles */

	/* UBWC 6.0, hbb 16 - 13 = 3 (hi 0, lo 3), swizzle levels 2 and 3 on, version tag 5,
	 * yuvnotcomptofc, rgb565 predicator, amsbc; uavflagprd_inv 2; ubwc_mode 0. */
	gw_pipe(PIPE_BV, 0x8700, 0x60);         /* GRAS_NC_MODE_CNTL */
	gw_pipe(PIPE_BR, 0x8700, 0x60);
	gw_pipe(PIPE_BR, 0x8e08, 0x46);         /* RB_CCU_NC_MODE_CNTL */
	gw_pipe(PIPE_BR, 0x8f00, 0x28046);      /* RB_CMP_NC_MODE_CNTL */
	gw(CP_APERTURE_CNTL_HOST, 0);
	gw(0xae02, 0x26);                       /* SP_NC_MODE_CNTL */
	gw(0xb604, 0x06);                       /* TPL1_NC_MODE_CNTL */

	for (unsigned i = 0; i < 20; i++) {
		gw(0xb606 + i, a8xx_bicubic[i]);   /* TPL1_BICUBIC_WEIGHTS_TABLE(i) */
	}
	gw(0xe11, (1u << 7) | 1);               /* UCHE_CLIENT_PF */

	for (uint32_t pipe = PIPE_NONE; pipe <= PIPE_BV; pipe++) {
		gw(CP_APERTURE_CNTL_HOST, CP_APERTURE_PIPE(pipe));
		for (unsigned i = 0; i < sizeof(a840_nonctxt) / sizeof(a840_nonctxt[0]); i++) {
			if (a840_nonctxt[i].pipes & (1u << pipe)) {
				gw(a840_nonctxt[i].off, a840_nonctxt[i].val);
			}
		}
	}
	gw(CP_APERTURE_CNTL_HOST, 0);

	gw(0x2f, (1u << 30) | 0xcfffff);        /* RBBM_INTERFACE_HANG_INT_CNTL */
	gw(0x58f, 1u << 30);                    /* RBBM_SLICE_INTERFACE_HANG_INT_CNTL */

	/* a8xx_set_cp_protect: protection on, fault on violation, last span to the end. */
	const uint32_t cntl = 0xffff000b, n = sizeof(a840_protect) / sizeof(a840_protect[0]);
	gw_pipe(PIPE_BR, 0x84e, cntl);          /* CP_PROTECT_CNTL_PIPE */
	gw_pipe(PIPE_BV, 0x84e, cntl);
	gw(CP_APERTURE_CNTL_HOST, 0);
	for (unsigned i = 0; i < n; i++) {
		gw(0x850 + i, a840_protect[i]);     /* CP_PROTECT_GLOBAL(i) */
	}
	gw_pipe(PIPE_BR, 0x8af, a840_protect[n - 1]);  /* CP_PROTECT_PIPE(15) */
	gw_pipe(PIPE_BV, 0x8af, a840_protect[n - 1]);

	gw_pipe(PIPE_BR, 0x8e50, 1);            /* RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_ENABLE */
	static const uint32_t pipes[] = { 1, 2, 4, 5, 6, 7 };
	for (unsigned i = 0; i < sizeof(pipes) / sizeof(pipes[0]); i++) {
		gw_pipe(pipes[i], 0x8b8, 0x7e83f7f);   /* CP_INTERRUPT_STATUS_MASK_PIPE */
		gw_pipe(pipes[i], 0x8b4, 0x77f);       /* CP_HW_FAULT_STATUS_MASK_PIPE */
	}
	gw(CP_APERTURE_CNTL_HOST, 0);
	gw(0x8b6, 0x7f007f);                    /* CP_INTERRUPT_STATUS_MASK_GLOBAL */
	gw(0x62, 0x33d283c2);                   /* RBBM_INT_0_MASK (A8XX_INT_MASK) */

	gw_pipe(PIPE_BR, 0x8e09, 0x12000000);   /* RB_GC_GMEM_PROTECT, after the zap */
	gw(CP_APERTURE_CNTL_HOST, 0);
	gw(0x3c00, gr(0x3c00) & ~1u);           /* GBIF_CX_CONFIG: clock gating stays off */

	/* Read back, one aperture at a time (argument order is unspecified in C). */
	uint32_t br_gras = gr_pipe(PIPE_BR, 0x8700), br_ccu = gr(0x8e08), br_cmp = gr(0x8f00);
	uint32_t br_prot = gr(0x84e), br_gmemp = gr(0x8e09);
	uint32_t bv_prot = gr_pipe(PIPE_BV, 0x84e);
	gw(CP_APERTURE_CNTL_HOST, 0);
	printf("PANDORA GPU: hw_init rest: gmem min %08x%08x trap %08x%08x ubwc gras %x ccu %x cmp %x sp %x tpl1 %x\n",
	    gr(0xf09), gr(0xf08), gr(0xe09), gr(0xe08), br_gras, br_ccu, br_cmp, gr(0xae02), gr(0xb604));
	printf("PANDORA GPU: hw_init rest: protect cntl br %08x bv %08x global[0] %08x [%u] %08x gmem_protect %08x "
	    "int masks %08x %08x uche_mode %08x gbif_cx %08x\n", br_prot, bv_prot, gr(0x850), n - 1, gr(0x850 + n - 1),
	    br_gmemp, gr(0x62), gr(0x8b6), gr(0xe01), gr(0x3c00));
}

/* Level 14: hold OOB GPU_SET, open the user interface, and use it once from the kernel the way
 * the user client will: a buffer, mapped at the window start, an IB in it, submit, wait, free.
 * With level 15 asked for, the rest of hw_init comes first, before any client can open. */
static bool
gpu_level14(bool rest)
{
	bool ok;
	gmu_wr(GMU_HOST2GMU_INTR_SET, 1u << 30);
	(void)gpu_poll(gpu_gmu, GMU_REG(GMU_GMU2HOST_INTR_INFO), 1u << 31, 1u << 31, 100000, &ok);
	gmu_wr(GMU_GMU2HOST_INTR_CLR, 1u << 31);
	if (!ok) {
		printf("PANDORA GPU: OOB GPU_SET NOT ACKED\n");
		return false;
	}
	if (rest) {
		gpu_level15();
	}
	/* a8xx_gpu_get_slice_info: the chip id user space sees carries the enabled slice count in
	 * bits 4..7 (CX_MISC_SLICE_ENABLE_FINAL, max_slices 3 for A840); Mesa's table has 0x44050a31. */
	uint32_t slices = gpu_rd(gpu_gmu, 0x03d9e000u + 0x87u * 4 - GMU_PA) & 7u;
	gpu_chip_id = 0x44050a01u | ((uint32_t)__builtin_popcount(slices) << 4);
	printf("PANDORA GPU: slice mask %x, chip id %08x\n", slices, gpu_chip_id);
	gpu_ready = true;
	gpu_step("user interface: kernel self-test through it");
	int c = pandora_gpu_open();
	uint64_t h = 0, ws = 0, wsz = 0, fence = 0;
	int err = c < 0 ? ENXIO : pandora_gpu_bo_alloc(c, 0x10000, 12, &h);
	if (!err) {
		pandora_gpu_window(c, &ws, &wsz);
		err = pandora_gpu_bo_map(c, h, ws, 3);
	}
	printf("PANDORA GPU: self-test client %d window %#llx+%#llx buffer %#llx -> %d\n", c, ws, wsz, h, err);
	bool good = false;
	if (!err) {
		const uint64_t boff = gpu_bos[(uint32_t)h - 1].off;
		vm_offset_t va = gpu_pool_map(boff, 0x10000);
		uint32_t *ib = (uint32_t *)va;
		ib[0] = pkt7(0x3d, 3);
		ib[1] = (uint32_t)(ws + 0x8000);
		ib[2] = (uint32_t)((ws + 0x8000) >> 32);
		ib[3] = 0x14141414;
		gpu_pool_sync(gpu_bos[(uint32_t)h - 1].off, 16, false);
		const uint64_t ibva = ws, ibbytes = 16;
		err = pandora_gpu_submit(c, &ibva, &ibbytes, 1, &fence);
		int w = err ? err : pandora_gpu_wait(fence, 1000000000ull);
		gpu_pool_sync(gpu_bos[(uint32_t)h - 1].off + 0x8000, 64, true);
		uint32_t v = *(const volatile uint32_t *)(va + 0x8000);
		good = !err && !w && v == 0x14141414;
		gpu_pool_unmap(va, boff, 0x10000);
		printf("PANDORA GPU: self-test submit -> %d fence %llu wait -> %d, word %08x; timestamp %llu\n",
		    err, fence, w, v, pandora_gpu_timestamp());
		(void)ring_idle(10);
	}
	if (c >= 0) {
		int f = h ? pandora_gpu_bo_free(c, h) : 0;
		printf("PANDORA GPU: self-test free -> %d\n", f);
		pandora_gpu_close(c);
	}
	return good;
}

/*
 * Patch 0193: the last GPU_HIST submissions (pandora_gpu_submit), and a dump of the CP when the
 * ring timestamp stops short of the last fence issued (bb19: backboardd froze, its waits timed
 * out until the end, and the log ring had no room left for what the GPU was doing).
 */
/* 0194: the dwords of an IB around where the CP fetches (INIT - REM dwords in; it fetches ahead
 * of what it runs), from the pool buffer that holds it. Under the lock. */
static void
gpu_hang_ib(const char *tag, uint64_t base, uint32_t init, uint32_t rem)
{
	if (!base || rem > init) {
		return;
	}
	const uint32_t at = init - rem, from = at > 40 ? at - 40 : 0, to = MIN(init, at + 8);
	for (unsigned j = 0; j < GPU_MAX_BOS; j++) {
		const struct gpu_bo *b = &gpu_bos[j];
		if (!b->used || !b->iova || b->pages || base < b->iova || base + (uint64_t)to * 4 > b->iova + b->bytes) {
			continue;
		}
		const uint64_t off = b->off + (base - b->iova) + (uint64_t)from * 4, len = (uint64_t)(to - from) * 4;
		gpu_pool_sync(off, len, true);
		vm_offset_t va = gpu_pool_map(off, len);
		if (!va) {
			return;
		}
		for (uint32_t i = from; i < to; i += 8) {
			char line[128];
			int n = snprintf(line, sizeof(line), "PANDORA GPU HANG: %s +%u:", tag, i);
			for (uint32_t k = i; k < MIN(to, i + 8) && n > 0 && n < (int)sizeof(line); k++) {
				n += snprintf(line + n, sizeof(line) - n, "%s%08x", k == at ? " >" : " ",
				    ((const volatile uint32_t *)va)[k - from]);
			}
			printf("%s\n", line);
		}
		gpu_pool_unmap(va, off, len);
		return;
	}
	printf("PANDORA GPU HANG: %s %llx not in a pool buffer\n", tag, (unsigned long long)base);
}

static void
gpu_hang_dump(uint32_t ring)
{
	printf("PANDORA GPU HANG: ring timestamp %u, issued %u, rbbm %08x int0 %08x cp int %08x "
	    "rptr %u rptr bv %u wptr %u\n", ring, gpu_seqno, gr(RBBM_STATUS_A8XX), gr(RBBM_INT_0_STATUS),
	    gr(CP_INTERRUPT_STATUS_GLOBAL), gr(CP_RB_RPTR), gr(CP_RB_RPTR_BV), ring_wptr);
	for (uint32_t pipe = 1; pipe <= 2; pipe++) {
		gw(CP_APERTURE_CNTL_HOST, CP_APERTURE_PIPE(pipe));
		printf("PANDORA GPU HANG: %s IB1 base %08x%08x rem %08x init %08x hw fault %08x int %08x sqe %08x\n",
		    pipe == 1 ? "BR" : "BV", gr(CP_IB1_BASE_PIPE + 1), gr(CP_IB1_BASE_PIPE),
		    gr(CP_IB1_REM_SIZE_PIPE), gr(CP_IB1_INIT_SIZE_PIPE), gr(CP_HW_FAULT_STATUS_PIPE),
		    gr(CP_INTERRUPT_STATUS_PIPE), gr(CP_SQE_STATUS_PIPE));
		/* 0194: IB2 and IB3 (a6xx.xml A8XX: CP_IB2_BASE 0x904, REM 0x906, INIT 0x907, CP_IB3_BASE 0x908),
		 * then the commands each level fetches. */
		const uint64_t ib1 = ((uint64_t)gr(CP_IB1_BASE_PIPE + 1) << 32) | gr(CP_IB1_BASE_PIPE);
		const uint64_t ib2 = ((uint64_t)gr(CP_IB2_BASE_PIPE + 1) << 32) | gr(CP_IB2_BASE_PIPE);
		const uint64_t ib3 = ((uint64_t)gr(CP_IB3_BASE_PIPE + 1) << 32) | gr(CP_IB3_BASE_PIPE);
		const uint32_t r1 = gr(CP_IB1_REM_SIZE_PIPE), i1 = gr(CP_IB1_INIT_SIZE_PIPE);
		const uint32_t r2 = gr(CP_IB2_REM_SIZE_PIPE), i2 = gr(CP_IB2_INIT_SIZE_PIPE);
		const char *name = pipe == 1 ? "BR" : "BV";
		printf("PANDORA GPU HANG: %s IB2 base %llx rem %08x init %08x IB3 base %llx\n", name,
		    (unsigned long long)ib2, r2, i2, (unsigned long long)ib3);
		char tag[16];
		snprintf(tag, sizeof(tag), "%s IB1", name);
		gpu_hang_ib(tag, ib1, i1, r1);
		snprintf(tag, sizeof(tag), "%s IB2", name);
		gpu_hang_ib(tag, ib2, i2, r2);
	}
	gw(CP_APERTURE_CNTL_HOST, 0);
	for (uint32_t k = 0; k < GPU_HIST; k++) {
		const struct gpu_hist *h = &gpu_hist[(gpu_seqno + 1 + k) % GPU_HIST];
		if (h->seq) {
			printf("PANDORA GPU HANG: fence %u%s client %d thread %u ibs %u first %08x ib0 %llx+%u ibn %llx+%u\n",
			    h->seq, (int32_t)(h->seq - ring) > 0 ? " (pending)" : "", h->client, h->thread, h->n,
			    h->first, (unsigned long long)h->ib0, h->dw0, (unsigned long long)h->ibn, h->dwn);
		}
	}
}

/*
 * Patch 0189, with pandora_gpu_metal=1: every 5 s for 10 minutes (0190), what each client has done
 * (allocations, imports, mappings, frees, submissions, its last fence), the ring timestamp, and
 * any GPU SMMU context fault (FSR, FAR, FSYNR0 of bank 0; cleared once logged); a line only when
 * something changed. Which process renders through the GPU, and whether the GPU faults on it.
 */
static void
gpu_watch(void)
{
	char last[GPU_MAX_CLIENTS][96] = {};
	uint32_t last_ring = ~0u, last_fsr = 0, dumped = ~0u, last_issued = 0;
	for (unsigned t = 0; ; t++) {   /* 0193: as long as the system runs (lines only on change) */
		gpu_sleep(5000);
		lck_mtx_lock(&gpu_lock);
		for (unsigned i = 0; i < GPU_MAX_CLIENTS; i++) {
			char now[96];
			if (!gpu_clients[i].used) {
				snprintf(now, sizeof(now), "-");
			} else {
				snprintf(now, sizeof(now), "%s alloc %u import %u map %u free %u submit %u fence %u",
				    gpu_clients[i].name, gpu_clients[i].allocs, gpu_clients[i].imports, gpu_clients[i].maps,
				    gpu_clients[i].frees, gpu_clients[i].submits, gpu_clients[i].fence);
			}
			if (strcmp(now, last[i]) != 0) {
				printf("PANDORA GPU WATCH: %u s client %u: %s\n", (t + 1) * 5, i, now);
				strlcpy(last[i], now, sizeof(last[i]));
			}
		}
		const volatile uint32_t *mem = (const volatile uint32_t *)(gpu_bufs[2].va + MEMPTRS_OFF);
		flush_dcache64((addr64_t)gpu_bufs[2].pa + MEMPTRS_OFF, 16, TRUE);
		uint32_t ring = mem[1], fsr = gpu_rd(gpu_smmu_va, gpu_smmu.cb + 0x58);
		if (ring != last_ring || fsr != last_fsr) {
			printf("PANDORA GPU WATCH: %u s ring timestamp %u (issued %u) rbbm %08x cp int %08x rptr %u wptr %u\n",
			    (t + 1) * 5, ring, gpu_seqno, gr(RBBM_STATUS_A8XX), gr(CP_INTERRUPT_STATUS_GLOBAL),
			    gr(CP_RB_RPTR), ring_wptr);
			last_ring = ring;
		} else if ((int32_t)(last_issued - ring) > 0 && ring != dumped) {
			/* 0196: work that was already pending at the last tick, no progress since (bb22's
			 * dump caught a submission issued just before an idle tick). */
			gpu_hang_dump(ring);   /* 5 s and more without progress, work pending */
			dumped = ring;
		}
		if (fsr && fsr != last_fsr) {
			printf("PANDORA GPU WATCH: SMMU fault fsr %08x far %08x%08x fsynr0 %08x\n", fsr,
			    gpu_rd(gpu_smmu_va, gpu_smmu.cb + 0x64), gpu_rd(gpu_smmu_va, gpu_smmu.cb + 0x60),
			    gpu_rd(gpu_smmu_va, gpu_smmu.cb + 0x68));
			gpu_wr(gpu_smmu_va, gpu_smmu.cb + 0x58, fsr);
		}
		last_fsr = fsr;   /* bit 10 stays set from boot on (bb2 log); log changes only */
		last_issued = gpu_seqno;
		lck_mtx_unlock(&gpu_lock);
	}
}

/*
 * Patch 0198 (first version 0197): GPU frequency scaling. The busy counter of Linux a8xx_gpu_busy: the
 * GMU's CX power counter 0 counts XO (19.2 MHz) ticks while the GPU is busy (a8xx hw_init:
 * GPU_GMU_AO_GPU_CX_BUSY_MASK 0xff000000, POWER_COUNTER_SELECT_XOCLK_0 countable 5, POWER_COUNTER_ENABLE).
 * 0197's simple_ondemand at 90/5 every 50 ms swung 160 <-> 1200 MHz (bb25: up to 109 changes in 10 s; a
 * frame that keeps 160 MHz 95 % busy keeps 1200 MHz 13 % busy, and 13 % at 85 % asked for 180 MHz).
 * Now, after Linux msm_gpu_devfreq (upthreshold 50, downdifferential 10, boost on activity after
 * idle) and KGSL (10 ms windows, a 50 ms busy ceiling, 80 ms idle timeout, the phone DT's per-level
 * bus-freq of bin qcom,gpu-pwrlevels-1, initial level 222 MHz):
 *   - every 16 ms (one frame), the frequency that would have kept the window 50 % busy;
 *   - up at once to the lowest level at or above 1.1 x that; to the top after a window over 85 %
 *     busy or 50 ms busy without a break;
 *   - down one level only after 4 windows in a row (one more right after a step up) that would
 *     have needed at most 0.8 x the next lower level; windows without work do not count;
 *   - 80 ms without work: back to the initial level 222 MHz;
 *   - work after 50 ms or more without a submission (pandora_gpu_submit wakes this thread): a floor
 *     of max(2 x the current frequency, 578 MHz) for 50 ms (msm_devfreq_active's boost);
 *   - bandwidth: the level's DT bus-freq index, in the same HFI_H2F_MSG_GX_BW_PERF_VOTE.
 * Off with pandora_gpu_perf (a fixed level, 0196); pandora_gpu_perf_max caps it (default 18). A line
 * every 10 s: busy share, the levels used, changes, boosts. No temperature limit yet.
 */
#define GMU_AO_GPU_CX_BUSY_MASK       0x23b0eu
#define GMU_POWER_COUNTER_ENABLE_A8   0x1fc10u
#define GMU_POWER_COUNTER_SELECT_XO0  0x1fc40u
#define GMU_POWER_COUNTER_XO0_L       0x1fca0u
#define GMU_POWER_COUNTER_XO0_H       0x1fca1u
#define DCVS_TICK_MS        16u
#define DCVS_INITIAL        3u          /* qcom,initial-pwrlevel 15 of 18 (descending) = 222 MHz */
#define DCVS_BOOST_MIN_HZ   578000000u
#define DCVS_BOOST_NS       (50ull * NSEC_PER_MSEC)
#define DCVS_IDLE_NS        (80ull * NSEC_PER_MSEC)
#define DCVS_CEILING_NS     (50ull * NSEC_PER_MSEC)

/* qcom,gpu-pwrlevels-1 qcom,bus-freq per level, in gpu_opps order (160 .. 1200 MHz). */
static const uint8_t gpu_opp_bw[] = { 1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 5, 5, 7, 7, 9, 9, 11, 11 };

static uint64_t
gpu_busy_ticks(void)
{
	uint32_t hi = gmu_rd(GMU_POWER_COUNTER_XO0_H), lo = gmu_rd(GMU_POWER_COUNTER_XO0_L);
	uint32_t hi2 = gmu_rd(GMU_POWER_COUNTER_XO0_H);
	if (hi2 != hi) {
		lo = gmu_rd(GMU_POWER_COUNTER_XO0_L);
	}
	return ((uint64_t)hi2 << 32) | lo;
}

/* The last fence the ring retired (memptrs, as gpu_watch reads it). */
static uint32_t
gpu_retired(void)
{
	const volatile uint32_t *mem = (const volatile uint32_t *)(gpu_bufs[2].va + MEMPTRS_OFF);
	flush_dcache64((addr64_t)gpu_bufs[2].pa + MEMPTRS_OFF, 16, TRUE);
	return mem[1];
}

static bool
gpu_vote(uint32_t perf)
{
	static uint32_t vote[4];
	vote[1] = 1;            /* ack_type: blocking */
	vote[2] = perf;
	vote[3] = gpu_opp_bw[perf - 1];
	return hfi_send(HFI_H2F_MSG_GX_BW_PERF_VOTE, vote, 4);
}

/* The lowest level up to TOP whose frequency is at least HZ (TOP when none is). */
static uint32_t
gpu_level_for(uint64_t hz, uint32_t top)
{
	uint32_t l = 1;
	while (l < top && gpu_opps[l - 1].hz < hz) {
		l++;
	}
	return l;
}

static void
gpu_dcvs_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	uint32_t top = NGPU - 1, cur = 1;
	(void)PE_parse_boot_argn("pandora_gpu_perf_max", &top, sizeof(top));
	top = MAX(1u, MIN(top, (uint32_t)NGPU - 1));
	_Static_assert(sizeof(gpu_opp_bw) == NGPU - 1, "a bus-freq index per level");
	gmu_wr(GMU_AO_GPU_CX_BUSY_MASK, 0xff000000u);
	gmu_wr(GMU_POWER_COUNTER_SELECT_XO0, (gmu_rd(GMU_POWER_COUNTER_SELECT_XO0) & ~0xffu) | (1u << 5));
	gmu_wr(GMU_POWER_COUNTER_ENABLE_A8, 1);
	const uint32_t initial = MIN(DCVS_INITIAL, top);
	if (gpu_vote(initial)) {
		cur = initial;
	}
	printf("PANDORA GPU DCVS: on, levels 1..%u (%u..%u MHz), every %u ms, from %u MHz\n", top,
	    gpu_opps[0].hz / 1000000, gpu_opps[top - 1].hz / 1000000, DCVS_TICK_MS, gpu_opps[cur - 1].hz / 1000000);
	uint64_t last_busy = gpu_busy_ticks(), last_t = mach_absolute_time(), idle_since = last_t;
	uint64_t run_ns = 0, boost_until = 0, boost_hz = 0;
	uint64_t win_busy = 0, win_ns = 0, at_level[NGPU] = {};
	uint32_t down = 0, hold = 0, changes = 0, failed = 0, boosts = 0;
	for (;;) {
		uint64_t deadline;
		clock_interval_to_deadline(DCVS_TICK_MS, NSEC_PER_MSEC, &deadline);
		(void)assert_wait_deadline(&gpu_dcvs_chan, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
		const uint64_t busy = gpu_busy_ticks(), t = mach_absolute_time();
		uint64_t ns, since;
		absolutetime_to_nanoseconds(t - last_t, &ns);
		const uint64_t busy_ns = MIN(ns, (busy - last_busy) * 625 / 12);   /* 52.083 ns per XO tick */
		last_busy = busy;
		last_t = t;
		if (!ns) {
			continue;
		}
		win_busy += busy_ns;
		win_ns += ns;
		at_level[cur] += ns;
		const bool pending = (int32_t)(gpu_seqno - gpu_retired()) > 0;
		const bool active = busy_ns > 0 || pending;
		/* Work after a gap without submissions: pandora_gpu_submit set the kick and woke us. */
		if (__atomic_exchange_n(&gpu_dcvs_kick, 0, __ATOMIC_RELAXED)) {
			boost_hz = MAX(2ull * gpu_opps[cur - 1].hz, (uint64_t)DCVS_BOOST_MIN_HZ);
			absolutetime_to_nanoseconds(t, &since);
			boost_until = since + DCVS_BOOST_NS;
			boosts++;
		}
		absolutetime_to_nanoseconds(t, &since);
		const uint32_t floor = since < boost_until ? gpu_level_for(boost_hz, top) : 1;
		/* The frequency that would have kept this window 50 % busy. */
		const uint64_t need = (uint64_t)busy_ns * gpu_opps[cur - 1].hz * 2 / ns;
		run_ns = busy_ns * 100 >= ns * 95 ? run_ns + ns : 0;
		uint32_t want = cur;
		if (busy_ns * 100 > ns * 85 || run_ns >= DCVS_CEILING_NS) {
			want = top;
		} else if (gpu_level_for(need * 11 / 10, top) > cur) {
			want = gpu_level_for(need * 11 / 10, top);
		} else if (active) {
			if (cur > 1 && need * 10 <= (uint64_t)gpu_opps[cur - 2].hz * 8) {
				if (++down >= 4 + hold) {
					want = cur - 1;
				}
			} else {
				down = 0;
			}
		} else {
			uint64_t idle;
			absolutetime_to_nanoseconds(t - idle_since, &idle);
			if (idle >= DCVS_IDLE_NS && cur != initial) {
				want = initial;
			}
		}
		if (active) {
			idle_since = t;
		}
		want = MAX(want, floor);
		if (want != cur) {
			if (gpu_vote(want)) {
				hold = want > cur ? 1 : 0;
				cur = want;
				changes++;
			} else {
				failed++;
			}
			down = 0;
		}
		if (win_ns >= 10ull * NSEC_PER_SEC) {
			char used[200];
			int n = 0;
			used[0] = 0;
			for (uint32_t l = 1; l < NGPU && n >= 0 && n < (int)sizeof(used); l++) {
				if (at_level[l] * 100 / win_ns >= 1) {
					n += snprintf(used + n, sizeof(used) - n, " %u:%llu%%", gpu_opps[l - 1].hz / 1000000,
					    at_level[l] * 100 / win_ns);
				}
				at_level[l] = 0;
			}
			printf("PANDORA GPU DCVS: busy %llu%%, MHz:time%s, %u changes, %u boosts, %u failed, now %u MHz\n",
			    win_busy * 100 / win_ns, used, changes, boosts, failed, gpu_opps[cur - 1].hz / 1000000);
			win_busy = win_ns = 0;
			changes = failed = boosts = 0;
		}
	}
}

static void
gpu_thread(void *arg, wait_result_t result)
{
	int level = 0;
	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("pandora_gpu", &level, sizeof(level));
	printf("PANDORA GPU: level %d (1 = read only, 2 = CX power and SMMU clock, "
	    "3 = GPLL0 vote and GPU_INIT_REGS, 4 = GMU SMMU context, 5 = GMU cold start, 6 = HFI perf table, 7 = HFI start, 8 = GX on, 9 = GPU SMMU context, 10 = CP, 11 = zap, 12 = indirect buffer, 13 = pool and framed submit, 14 = user interface, 15 = rest of hw_init before it)\n", level);
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
													if (i && level >= 13) {
														bool s = gpu_level13();
														printf("PANDORA GPU: level 13 (pool, framed submit) %s\n",
														    s ? "done" : "stopped");
														if (s && level >= 14) {
															bool u = gpu_level14(level >= 15);
															printf("PANDORA GPU: level 14 (user interface) %s\n",
															    u ? "done" : "stopped");
															int metal = 0;
															(void)PE_parse_boot_argn("pandora_gpu_metal", &metal, sizeof(metal));
															uint32_t fixed = 0;
															if (u && !PE_parse_boot_argn("pandora_gpu_perf", &fixed, sizeof(fixed))) {
																thread_t dcvs;   /* 0197 */
																if (kernel_thread_start(gpu_dcvs_thread, NULL, &dcvs) == KERN_SUCCESS) {
																	thread_deallocate(dcvs);
																}
															}
															if (u && metal) {
																gpu_watch();
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
	/* Before launchd: by level 13's turn userland has run for a minute and the largest free
	 * run was between 128 and 256 MiB (gpu13 log). */
	if (level >= 13) {
		(void)gpu_pool_reserve();
	}
	if (kernel_thread_start(gpu_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */

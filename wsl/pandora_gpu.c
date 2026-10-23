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

extern kern_return_t thread_terminate(thread_t);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
void pandora_gpu_init(void);

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

static void
gpu_thread(void *arg, wait_result_t result)
{
	int level = 0;
	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("pandora_gpu", &level, sizeof(level));
	printf("PANDORA GPU: level %d (1 = read only, 2 = CX power and SMMU clock)\n", level);
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

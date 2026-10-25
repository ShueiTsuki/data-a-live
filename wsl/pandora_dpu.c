/*
 * PANDORA display controller, read only (patch 0215): what the ABL left running, before the display
 * scans out the GPU's frames itself (docs/GPU_PLAN.md item 9). Nothing is written.
 *
 * SM8850's MDSS has a DPU 13.0 (qcom,sde-hw-version 0xd0000000). Register names and offsets are the ones
 * of Qualcomm's mainline Linux driver (drivers/gpu/drm/msm/disp/dpu1: catalog/dpu_13_0_kaanapali.h,
 * dpu_hw_sspp_v13.c, dpu_hw_ctl.c, dpu_hw_lm.c, dpu_hw_intf.c, dpu_hw_interrupts.c). Catalog offsets
 * count from the "mdp" region, MDSS + 0x1000; the phone's DT (qcom,sde-*-off) counts from MDSS and
 * gives the same addresses. On v13 a source pipe's registers sit in its rectangle blocks REC0 (+0x1000)
 * and REC1 (+0x3000), not at the pipe's base, which is why step 1's reads at the DT offsets saw zeros.
 *
 * Only registers inside the DT's block sizes (qcom,sde-*-size) or ones mainline reads on v13 are touched.
 * Mappings stay (a few pages, once): r1b panicked in ml_io_unmap ("Attempting to remove permanent VM map entry").
 *
 * Logged once, from the framebuffer's start (boot-arg pandora_dpu_probe=0 skips it):
 *   - MDSS and DPU versions;
 *   - each control path (CTL) in use: interfaces, pipes and mixers it drives;
 *   - each source pipe with an address or size: format, sizes, position, stride, the address it fetches;
 *   - mixers 0-3 and the DSI interfaces (timing engine, command-mode trigger, tear check, autorefresh,
 *     interrupt enables), then a second look 100 ms later at the counters, to see the panel refresh;
 *   - the apps SMMU route of the display's stream (DT qcom,smmu_sde_unsec_cb: iommus 0x800 mask 0x2):
 *     stream match, context bank, its translation, and the walk of the address the active pipe fetches.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <machine/machine_routines.h>

#if defined(PANDORA)

#include <vm/pmap.h>

/*
 * Mappings come from ml_io_map_unmappable (kernel_map), not ml_io_map: ml_io_map takes permanent space in the small
 * io_submap, and the 1 MiB SMMU window at the first frame ran it out (r2c: kmem_alloc ... failed with 3, a panic).
 */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
#define dpu_map(pa, len) ml_io_map_unmappable((vm_offset_t)(pa), (vm_size_t)(len), VM_WIMG_IO)
extern void IOSleep(unsigned int milliseconds);

#define DPU_MDSS_PA   0x09800000u
#define DPU_MDSS_LEN  0x001c8800u
#define DPU_MDP       0x1000u                       /* the catalog's base */
#define DPU_CTL(n)    (DPU_MDP + 0x1f000u + 0x1000u * (n))
#define DPU_LM(n)     (DPU_MDP + 0x103000u + 0x8000u * (n))
#define DPU_INTF(n)   (DPU_MDP + 0x18d000u + 0x1000u * (n))   /* tear check and autorefresh are here on v13 */
#define DPU_TEARI(n)  (DPU_MDP + 0x18d800u + 0x1000u * (n))
#define DPU_REC0      0x1000u
#define DPU_REC1      0x3000u
#define DPU_SMMU_PA   0x15000000u
#define DPU_SID       0x800u

static const struct {
	const char *name;
	uint32_t base;
} dpu_sspp[] = {
	{ "vig0", 0x2b000 }, { "vig1", 0x34000 }, { "vig2", 0x3d000 }, { "vig3", 0x46000 },
	{ "dma0", 0x97000 }, { "dma1", 0xa0000 }, { "dma2", 0xa9000 }, { "dma3", 0xb2000 },
	{ "dma4", 0xbb000 }, { "dma5", 0xc4000 },
};

static vm_offset_t dpu_va;

static inline uint32_t
dpu_rd(uint32_t off)
{
	return *(volatile uint32_t *)(dpu_va + off);
}

static inline uint32_t
io_rd(vm_offset_t va, uint32_t off)
{
	return *(volatile uint32_t *)(va + off);
}

static uint64_t
phys_rd64(uint64_t pa)
{
	vm_offset_t va = dpu_map(pa & ~0xfffull, 0x1000);
	if (!va) {
		return 0;
	}
	uint64_t v = *(volatile uint64_t *)(va + (pa & 0xff8));
	return v;
}

/* Stage-1 walk of IOVA with a 4 KB granule from TTBR0 (TCR.T0SZ): the output address, or ~0 when not mapped. */
static uint64_t
dpu_walk(uint64_t ttbr, uint32_t tcr, uint64_t iova, uint64_t *tables, unsigned *levels)
{
	const unsigned t0sz = tcr & 0x3f, bits = 64 - t0sz;
	unsigned level = bits > 39 ? 0 : bits > 30 ? 1 : 2;
	uint64_t table = ttbr & 0x0000fffffffff000ull;
	*levels = 0;
	if (((tcr >> 14) & 3) != 0) {
		return ~0ull;   /* not a 4 KB granule */
	}
	for (; level <= 3; level++) {
		const unsigned shift = 12 + 9 * (3 - level);
		const uint64_t d = phys_rd64(table + 8 * ((iova >> shift) & 0x1ff));
		tables[(*levels)++] = table;
		if ((d & 1) == 0) {
			return ~0ull;
		}
		if (level < 3 && (d & 2) == 0) {   /* block */
			return (d & 0x0000fffffffff000ull & ~((1ull << shift) - 1)) | (iova & ((1ull << shift) - 1));
		}
		if (level == 3) {
			return (d & 0x0000fffffffff000ull) | (iova & 0xfff);
		}
		table = d & 0x0000fffffffff000ull;
	}
	return ~0ull;
}

/* The apps SMMU route for the display stream, and where FETCH (an address an active pipe reads) lands. */
static void
dpu_smmu(uint64_t fetch, uint64_t fb)
{
	vm_offset_t gr0 = dpu_map(DPU_SMMU_PA, 0x1000);
	if (!gr0) {
		printf("PANDORA DPU: apps SMMU not mapped\n");
		return;
	}
	const uint32_t id1 = io_rd(gr0, 0x24), nsmr = io_rd(gr0, 0x20) & 0xff;
	const uint64_t page = (id1 & (1u << 31)) ? 0x10000 : 0x1000;
	const uint64_t numpage = 1ull << (((id1 >> 28) & 7) + 1);
	bool any = false;
	for (unsigned i = 0; i < nsmr; i++) {
		const uint32_t smr = io_rd(gr0, 0x800 + 4 * i);
		if (!(smr & 0x80000000u) || ((DPU_SID ^ smr) & ~(smr >> 16) & 0x7fff) != 0) {
			continue;
		}
		any = true;
		const uint32_t s2cr = io_rd(gr0, 0xc00 + 4 * i), type = (s2cr >> 16) & 3, cb = s2cr & 0xff;
		printf("PANDORA DPU: SMMU SMR %u %08x (id %#x mask %#x) S2CR %08x (%s, bank %u)\n", i, smr, smr & 0x7fff,
		    (smr >> 16) & 0x7fff, s2cr, type == 0 ? "translate" : type == 1 ? "bypass" : "fault", cb);
		if (type != 0) {
			continue;
		}
		vm_offset_t gr1 = dpu_map(DPU_SMMU_PA + page, 0x1000);
		vm_offset_t cbv = dpu_map(DPU_SMMU_PA + numpage * page + cb * page, 0x1000);
		if (!gr1 || !cbv) {
			printf("PANDORA DPU: SMMU bank %u not mapped\n", cb);
			continue;
		}
		const uint32_t cbar = io_rd(gr1, 4 * cb), cba2r = io_rd(gr1, 0x800 + 4 * cb);
		const uint32_t sctlr = io_rd(cbv, 0x0), tcr = io_rd(cbv, 0x30);
		const uint64_t ttbr0 = io_rd(cbv, 0x20) | (uint64_t)io_rd(cbv, 0x24) << 32;
		printf("PANDORA DPU: SMMU bank %u: cbar %08x cba2r %08x sctlr %08x actlr %08x tcr %08x tcr2 %08x ttbr0 %#llx "
		    "mair0 %08x mair1 %08x fsr %08x far %#llx\n", cb, cbar, cba2r, sctlr, io_rd(cbv, 0x4), tcr, io_rd(cbv, 0x10),
		    ttbr0, io_rd(cbv, 0x38), io_rd(cbv, 0x3c), io_rd(cbv, 0x58),
		    io_rd(cbv, 0x60) | (uint64_t)io_rd(cbv, 0x64) << 32);
		if ((sctlr & 1) && fetch) {
			uint64_t tables[4] = {};
			unsigned levels = 0;
			const uint64_t pa = dpu_walk(ttbr0, tcr, fetch, tables, &levels);
			printf("PANDORA DPU: SMMU walk %#llx -> %#llx (%s; tables %#llx %#llx %#llx %#llx); the boot framebuffer is %#llx\n",
			    fetch, pa, pa == ~0ull ? "not mapped" : pa == fetch ? "identity" : "translated", tables[0], tables[1],
			    tables[2], tables[3], fb);
		} else if (fetch) {
			printf("PANDORA DPU: SMMU bank %u has translation off: the pipe fetches %#llx as it is\n", cb, fetch);
		}
	}
	if (!any) {
		printf("PANDORA DPU: no SMMU stream match for %#x (of %u entries)\n", DPU_SID, nsmr);
	}
}

/* What changes in 100 ms: frame and line counters, the CTL flush, the address each active rectangle reads. */
static void
dpu_sample(const char *when, uint32_t pipes)
{
	printf("PANDORA DPU: %s: intf1 frame %u line %u tear-line %u status %08x intr %08x tear-intr %08x\n", when,
	    dpu_rd(DPU_INTF(1) + 0xac), dpu_rd(DPU_INTF(1) + 0xb0), dpu_rd(DPU_INTF(1) + 0x2b0),
	    dpu_rd(DPU_INTF(1) + 0x26c), dpu_rd(DPU_INTF(1) + 0x1c4), dpu_rd(DPU_TEARI(1) + 0x4));
	printf("PANDORA DPU: %s: intf2 frame %u line %u tear-line %u intr %08x tear-intr %08x | ctl0 flush %08x ctl1 flush %08x\n",
	    when, dpu_rd(DPU_INTF(2) + 0xac), dpu_rd(DPU_INTF(2) + 0xb0), dpu_rd(DPU_INTF(2) + 0x2b0), dpu_rd(DPU_INTF(2) + 0x1c4),
	    dpu_rd(DPU_TEARI(2) + 0x4), dpu_rd(DPU_CTL(0) + 0x18), dpu_rd(DPU_CTL(1) + 0x18));
	for (unsigned p = 0; p < sizeof(dpu_sspp) / sizeof(dpu_sspp[0]); p++) {
		if (pipes & (1u << p)) {
			const uint32_t b = DPU_MDP + dpu_sspp[p].base;
			printf("PANDORA DPU: %s: %s current %08x / %08x\n", when, dpu_sspp[p].name,
			    dpu_rd(b + DPU_REC0 + 0x64), dpu_rd(b + DPU_REC1 + 0x64));
		}
	}
}

void pandora_dpu_probe(uint64_t fb);
void
pandora_dpu_probe(uint64_t fb)
{
	int on = 1;
	PE_parse_boot_argn("pandora_dpu_probe", &on, sizeof(on));
	if (!on) {
		return;
	}
	dpu_va = dpu_map(DPU_MDSS_PA, DPU_MDSS_LEN);
	if (!dpu_va) {
		printf("PANDORA DPU: MDSS not mapped\n");
		return;
	}
	printf("PANDORA DPU: MDSS hw %08x, DPU top %08x %08x %08x %08x\n", dpu_rd(0), dpu_rd(DPU_MDP), dpu_rd(DPU_MDP + 4),
	    dpu_rd(DPU_MDP + 8), dpu_rd(DPU_MDP + 0xc));

	for (unsigned n = 0; n < 6; n++) {
		const uint32_t c = DPU_CTL(n);
		const uint32_t intf = dpu_rd(c + 0xf4), fetch = dpu_rd(c + 0xfc), pipe = dpu_rd(c + 0x12c);
		if (intf == 0 && fetch == 0 && pipe == 0) {
			continue;
		}
		printf("PANDORA DPU: ctl%u: top %08x flush %08x start %08x prepare %08x intf %08x master %08x fetch %08x "
		    "pipe %08x layer %08x merge3d %08x dsc %08x layer0 %08x\n", n, dpu_rd(c + 0x14), dpu_rd(c + 0x18),
		    dpu_rd(c + 0x1c), dpu_rd(c + 0xd0), intf, dpu_rd(c + 0x134), fetch, pipe, dpu_rd(c + 0x130),
		    dpu_rd(c + 0xe4), dpu_rd(c + 0xe8), dpu_rd(c + 0x0));
	}

	uint32_t pipes = 0;
	uint64_t fetch = 0;
	for (unsigned p = 0; p < sizeof(dpu_sspp) / sizeof(dpu_sspp[0]); p++) {
		const uint32_t b = DPU_MDP + dpu_sspp[p].base;
		for (unsigned r = 0; r < 2; r++) {
			const uint32_t q = b + (r ? DPU_REC1 : DPU_REC0);
			const uint32_t size = dpu_rd(q + 0x14), addr = dpu_rd(q + 0x4c), cur = dpu_rd(q + 0x64);
			if (size == 0 && addr == 0 && cur == 0) {
				continue;
			}
			pipes |= 1u << p;
			if (!fetch && cur) {
				fetch = cur;
			}
			printf("PANDORA DPU: %s rec%u: format %08x unpack %08x op %08x img %08x src %08x xy %08x out %08x "
			    "out-xy %08x addr %08x %08x stride %08x current %08x active %08x flush %08x\n", dpu_sspp[p].name, r,
			    dpu_rd(q + 0x0), dpu_rd(q + 0x4), dpu_rd(q + 0x8), dpu_rd(q + 0x10), size, dpu_rd(q + 0x18),
			    dpu_rd(q + 0x1c), dpu_rd(q + 0x20), addr, dpu_rd(q + 0x50), dpu_rd(q + 0x5c), cur,
			    dpu_rd(q + 0xc8), dpu_rd(q + 0xd4));
		}
		if (pipes & (1u << p)) {
			printf("PANDORA DPU: %s common: clk %08x multirect %08x addr-config %08x status %08x%s\n",
			    dpu_sspp[p].name, dpu_rd(b + 0x0), dpu_rd(b + 0x10), dpu_rd(b + 0x14), dpu_rd(b + 0x44),
			    p < 4 ? "" : " (dma: no scaler)");
			if (p < 4) {
				printf("PANDORA DPU: %s scaler: %08x %08x %08x %08x\n", dpu_sspp[p].name, dpu_rd(b + 0x2000),
				    dpu_rd(b + 0x2004), dpu_rd(b + 0x2008), dpu_rd(b + 0x200c));
			}
		}
	}
	if (!pipes) {
		printf("PANDORA DPU: no source pipe has an address or size\n");
	}

	for (unsigned n = 0; n < 4; n++) {
		const uint32_t m = DPU_LM(n);
		printf("PANDORA DPU: lm%u: op %08x out %08x bg-sel %08x border %08x %08x stage0 op %08x fg-sel %08x alpha %08x "
		    "stage1 op %08x fg-sel %08x\n", n, dpu_rd(m + 0x0), dpu_rd(m + 0x4), dpu_rd(m + 0x14), dpu_rd(m + 0x1c),
		    dpu_rd(m + 0x20), dpu_rd(m + 0x40), dpu_rd(m + 0x44), dpu_rd(m + 0x48), dpu_rd(m + 0x70), dpu_rd(m + 0x74));
	}
	for (unsigned n = 1; n <= 2; n++) {
		const uint32_t i = DPU_INTF(n);
		printf("PANDORA DPU: intf%u (dsi%u): engine %08x config %08x config2 %08x format %08x cmd-trigger %08x mux %08x "
		    "status %08x count-en %08x\n", n, n - 1, dpu_rd(i + 0x0), dpu_rd(i + 0x4), dpu_rd(i + 0x60), dpu_rd(i + 0x90),
		    dpu_rd(i + 0x84), dpu_rd(i + 0x25c), dpu_rd(i + 0x26c), dpu_rd(i + 0xa8));
		printf("PANDORA DPU: intf%u tear: vsync-sel %08x check %08x sync %08x height %08x init %08x start %08x rd-ptr %08x "
		    "autorefresh %08x | intr en %08x tear-intr en %08x\n", n, dpu_rd(i + 0x280), dpu_rd(i + 0x284), dpu_rd(i + 0x288),
		    dpu_rd(i + 0x28c), dpu_rd(i + 0x294), dpu_rd(i + 0x2a0), dpu_rd(i + 0x2a4), dpu_rd(i + 0x2b4), dpu_rd(i + 0x1c0),
		    dpu_rd(DPU_TEARI(n) + 0x0));
	}

	dpu_sample("t0", pipes);
	IOSleep(100);
	dpu_sample("t0+100ms", pipes);
	dpu_smmu(fetch, fb);
}

/*
 * Patch 0217: the display scans out iOS's frames itself, as an iPhone's display does, instead of a CPU copy of
 * every frame into the ABL's panel memory (2.4 ms a frame, r1c). iokit/Kernel/arm/SweetFramebuffer.cpp decides
 * per frame; this part owns the hardware:
 *
 * SMMU. The display stream (SID 0x800 mask 2) reaches memory through apps SMMU bank 2 with translation off,
 * so it can read only what lies at a 32-bit physical address, and iOS's surfaces are wired kernel pages above
 * 4 GiB. Mainline Linux keeps the DPU behind its SMMU with 32-bit IOVAs; here likewise: the display's own bank
 * gets 4K stage-1 tables (ArmSmmu4K) that map the ABL's panel memory (main and rear screen, both read now) at
 * the same addresses, and its translation is turned on; the stream's route is not touched. Surfaces are mapped
 * into 16 MiB slots from 0x80000000. ACTLR is the DT's for SID 0x800 (qcom,actlr 0x1). Any fault in the bank in
 * the first 100 ms turns translation off again (the ABL's state) and leaves the CPU copy.
 *
 * Pipes. Main screen: CTL0 -> INTF1 (DSI0), source split over LM0/LM1: VIG0 fills x 0-609 and VIG1 x 610-1219
 * of the 1220x2656 panel. A frame is shown by giving both pipes the surface: source rectangles, stride and
 * address in their REC0 blocks, and with sweetfb_fill the QSEED3 scaler (bilinear, no LUT, phase step
 * src/dst in 21-bit fixed point, QSEED4 preload 4/2), all as mainline dpu_hw_sspp_v13.c / dpu_plane.c program
 * them, then CTL0 FLUSH (VIG0 bit 0, VIG1 bit 1, CTL bit 17). Command mode with autorefresh latches the
 * flushed registers at the next frame (120 Hz). The ABL's values are kept and written back for the panel
 * memory (console, display off, frames the pipes cannot take).
 */
#include <sys/errno.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <arm/caches_internal.h>
#include "adreno/ArmSmmu4K.h"
#include "adreno/ArmSmmu4KContext.h"
#include "adreno/ArmSmmu4KProgram.h"
#include "adreno/ArmSmmu4KMmio.h"

extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);   /* as bsd/dev/pandora_gpu.c */
extern bool pandora_log_sync(void);                          /* bsd/dev/sweet_ufs.c: the log file written now */

#define SC_SMMU_BYTES     0x100000u          /* DT apps-smmu@15000000 reg */
#define SC_SMR_VALID      (1u << 31)
#define SC_ACTLR          0x1u               /* DT qcom,actlr <0x800 0x02 0x01> */
#define SC_TABLE_PAGES    192u
#define SC_SLOT_BASE      0x80000000u
#define SC_SLOT_BYTES     (16u << 20)
#define SC_SLOTS          16u
#define SC_PIPES          2u                 /* VIG0, VIG1 */
#define SC_FLUSH          ((1u << 0) | (1u << 1) | (1u << 17))
#define SC_PANEL_W        1220u
#define SC_HALF_W         610u
#define SC_PANEL_H        2656u
/* REC block registers (dpu_hw_sspp_v13.c) */
#define SC_OP_MODE        0x08u
#define SC_SRC_SIZE       0x14u
#define SC_SRC_XY         0x18u
#define SC_OUT_SIZE       0x1cu
#define SC_OUT_XY         0x20u
#define SC_PIX_EXT_LR     0x24u
#define SC_PIX_EXT_TB     0x28u
#define SC_PIX_EXT_LR_ODX 0x40u
#define SC_PIX_EXT_TB_ODX 0x44u
#define SC_SRC0_ADDR      0x4cu
#define SC_YSTRIDE0       0x5cu
#define SC_CURRENT0       0x64u
/* QSEED3 at pipe + 0x2000 (dpu_hw_util.c) */
#define SC_QSEED          0x2000u
#define SC_Q_OP_MODE      0x04u
#define SC_Q_STEP_Y_H     0x10u
#define SC_Q_STEP_Y_V     0x14u
#define SC_Q_STEP_UV_H    0x18u
#define SC_Q_STEP_UV_V    0x1cu
#define SC_Q_PRELOAD      0x20u
#define SC_Q_SRC_SIZE     0x40u
#define SC_Q_SRC_SIZE_UV  0x44u
#define SC_Q_DST_SIZE     0x48u
#define SC_Q_INIT_Y_H     0x90u
#define SC_Q_INIT_Y_V     0x94u
#define SC_Q_INIT_UV_H    0x98u
#define SC_Q_INIT_UV_V    0x9cu

static const uint32_t sc_rec[SC_PIPES] = { DPU_MDP + 0x2b000u + DPU_REC0, DPU_MDP + 0x34000u + DPU_REC0 };
static const uint32_t sc_qseed[SC_PIPES] = { DPU_MDP + 0x2b000u + SC_QSEED, DPU_MDP + 0x34000u + SC_QSEED };
static const uint32_t sc_save_regs[] = { SC_OP_MODE, SC_SRC_SIZE, SC_SRC_XY, SC_OUT_SIZE, SC_OUT_XY, SC_PIX_EXT_LR,
	                                 SC_PIX_EXT_TB, SC_PIX_EXT_LR_ODX, SC_PIX_EXT_TB_ODX, SC_SRC0_ADDR, SC_YSTRIDE0 };
#define SC_SAVED (sizeof(sc_save_regs) / sizeof(sc_save_regs[0]))

static struct {
	bool ready;                     /* bank on, stream switched, pipes' panel values saved */
	bool on_panel;                  /* the pipes show the ABL's panel memory */
	vm_offset_t smmu;
	ArmSmmu4KMmio mmio;
	ArmSmmu4KIo io;
	ArmSmmu4KConfig tcfg;
	ArmSmmu4KResult tables;
	ArmSmmu4KContextConfig ctx;
	vm_offset_t tva;
	uint64_t tpa;
	unsigned tnext, bank, smr;
	uint32_t panel_s2cr, cb_off;
	uint32_t saved[SC_PIPES][SC_SAVED], saved_qop[SC_PIPES];
	uint32_t panel_addr;            /* what the pipes read before (0xfc800000) */
	bool slot_used[SC_SLOTS];
	uint64_t shows, faults;
} sc;

static inline void
dpu_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(dpu_va + off) = v;
}

static inline uint32_t
sc_rd(uint32_t off)
{
	return *(volatile uint32_t *)(sc.smmu + off);
}

static inline void
sc_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(sc.smmu + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* Map PAGES (4K physical pages) at IOVA in the bank's tables, read-only for the display; then TLBIALL. */
static bool
sc_map(uint64_t iova, const uint64_t *pages, uint64_t pa, uint32_t npages)
{
	const uint64_t pool = sc.tables.root_physical;
	const uint64_t attr = (1u << 11) | (1u << 10) | (2u << 8) | (1u << 7) | (1u << 6) | 3 | (3ull << 53);
	const unsigned in = sc.tcfg.input_bits, start = sc.tables.start_level;
	for (uint32_t i = 0; i < npages; i++) {
		const uint64_t va = iova + 0x1000ull * i, page = pages ? pages[i] : pa + 0x1000ull * i;
		uint64_t *t = (uint64_t *)sc.tva;
		for (unsigned level = start; level < 3; level++) {
			const unsigned shift = 39 - 9 * level, width = level == start ? in - shift : 9;
			uint64_t *e = &t[(va >> shift) & ((1u << width) - 1)];
			if (!*e) {
				if (sc.tnext >= SC_TABLE_PAGES) {
					printf("PANDORA SCANOUT: table pool full at iova %#llx\n", va);
					return false;
				}
				*e = (pool + 0x1000ull * sc.tnext++) | 3;
				clean_dcache64((addr64_t)(pool + ((vm_offset_t)e - sc.tva)), 8, TRUE);
			}
			t = (uint64_t *)(sc.tva + ((*e & 0xfffffffff000ull) - pool));
		}
		uint64_t *leaf = &t[(va >> 12) & 511];
		*leaf = (page & 0xfffffffff000ull) | attr;
		clean_dcache64((addr64_t)(pool + ((vm_offset_t)leaf - sc.tva)), 8, TRUE);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	ArmSmmu4KProgramResult r;
	return ArmSmmu4KInvalidateAllOwnedContext(&sc.io, sc.bank, &sc.ctx, 1000, &r) == ArmSmmu4KSuccess;
}

static void
sc_unmap(uint64_t iova, uint32_t npages)
{
	const uint64_t pool = sc.tables.root_physical;
	const unsigned in = sc.tcfg.input_bits, start = sc.tables.start_level;
	for (uint32_t i = 0; i < npages; i++) {
		const uint64_t va = iova + 0x1000ull * i;
		uint64_t *t = (uint64_t *)sc.tva;
		for (unsigned level = start; level < 3 && t; level++) {
			const unsigned shift = 39 - 9 * level, width = level == start ? in - shift : 9;
			const uint64_t e = t[(va >> shift) & ((1u << width) - 1)];
			t = e ? (uint64_t *)(sc.tva + ((e & 0xfffffffff000ull) - pool)) : NULL;
		}
		if (t) {
			t[(va >> 12) & 511] = 0;
			clean_dcache64((addr64_t)(pool + ((vm_offset_t)&t[(va >> 12) & 511] - sc.tva)), 8, TRUE);
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	ArmSmmu4KProgramResult r;
	(void)ArmSmmu4KInvalidateAllOwnedContext(&sc.io, sc.bank, &sc.ctx, 1000, &r);
}

/* Until CTL0 has taken the last flush (it clears its bits at the frame that latches them), at most 25 ms. */
static bool
sc_flush_idle(void)
{
	for (unsigned i = 0; i < 250; i++) {
		if ((dpu_rd(DPU_CTL(0) + 0x18) & SC_FLUSH) == 0) {
			return true;
		}
		delay(100);
	}
	return false;
}

int pandora_scanout_init(void);
int
pandora_scanout_init(void)
{
	int on = 1;
	PE_parse_boot_argn("pandora_scanout", &on, sizeof(on));
	if (!on) {
		printf("PANDORA SCANOUT: off (pandora_scanout=0): CPU copy\n");
		return ENOTSUP;
	}
	if (!dpu_va) {
		dpu_va = dpu_map(DPU_MDSS_PA, DPU_MDSS_LEN);
	}
	sc.smmu = dpu_map(DPU_SMMU_PA, SC_SMMU_BYTES);
	if (!dpu_va || !sc.smmu) {
		return ENOMEM;
	}
	/* What the main screen shows now: CTL0 drives VIG0 + VIG1 (fetch bits 16, 17) from one address. */
	const uint32_t a0 = dpu_rd(sc_rec[0] + SC_SRC0_ADDR), a1 = dpu_rd(sc_rec[1] + SC_SRC0_ADDR);
	if ((dpu_rd(DPU_CTL(0) + 0xfc) & 0x30000u) != 0x30000u || a0 == 0 || a0 != a1) {
		printf("PANDORA SCANOUT: main screen not as expected (ctl0 fetch %08x, vig0 %08x vig1 %08x): CPU copy\n",
		    dpu_rd(DPU_CTL(0) + 0xfc), a0, a1);
		return ENODEV;
	}
	/* Everything the display reads now must stay mapped at its address: main and rear screen (VIG2 on CTL1). */
	uint64_t lo = a0, hi = (uint64_t)a0 + (uint64_t)(dpu_rd(sc_rec[0] + SC_YSTRIDE0) & 0xffff) * SC_PANEL_H;
	const uint32_t rear = DPU_MDP + 0x3d000u + DPU_REC0, ra = dpu_rd(rear + SC_SRC0_ADDR);
	if (ra) {
		const uint64_t rend = (uint64_t)ra + (uint64_t)(dpu_rd(rear + SC_YSTRIDE0) & 0xffff) *
		    (dpu_rd(rear + SC_SRC_SIZE) >> 16);
		lo = MIN(lo, (uint64_t)ra);
		hi = MAX(hi, rend);
	}
	lo &= ~0xfffull;
	hi = (hi + 0xfff) & ~0xfffull;
	if (hi > SC_SLOT_BASE && lo < SC_SLOT_BASE + SC_SLOTS * SC_SLOT_BYTES) {
		printf("PANDORA SCANOUT: panel memory %#llx-%#llx inside the surface slots: CPU copy\n", lo, hi);
		return ENODEV;
	}
	sc.panel_addr = a0;
	printf("PANDORA SCANOUT: init at the first frame: panel memory %#llx-%#llx, main %#x\n", lo, hi, a0);
	(void)pandora_log_sync();

	/*
	 * The stream's route: its stream match and the bank its S2CR names (bank 2 from the ABL, stage 1 off, so the
	 * pipes read physical addresses). r2d: a free bank of the 81 could not be told for certain (the ones not named
	 * by an S2CR are not all reset), so no other bank is taken: the display's own bank gets the tables and its
	 * translation is turned on, the route stays as it is. Turning it back off (SCTLR as the ABL left it) is the
	 * way back to the old state.
	 */
	const uint32_t id0 = sc_rd(0x20), id1 = sc_rd(0x24), id2 = sc_rd(0x28), nsmr = id0 & 0xff;
	const uint64_t page = (id1 & (1u << 31)) ? 0x10000 : 0x1000, numpage = 1ull << (((id1 >> 28) & 7) + 1);
	sc.smr = ~0u;
	for (unsigned i = 0; i < nsmr; i++) {
		const uint32_t smr = sc_rd(0x800 + 4 * i), s2cr = sc_rd(0xc00 + 4 * i);
		if ((smr & SC_SMR_VALID) && (smr & 0x7fff) == DPU_SID && ((smr >> 16) & 0x7fff) == 0x2) {
			sc.smr = i;
			sc.panel_s2cr = s2cr;
		}
	}
	if (sc.smr == ~0u || ((sc.panel_s2cr >> 16) & 3) != 0) {
		printf("PANDORA SCANOUT: no translating stream match for %#x (%u, s2cr %08x): CPU copy\n", DPU_SID, sc.smr,
		    sc.panel_s2cr);
		return ENODEV;
	}
	sc.bank = sc.panel_s2cr & 0xff;
	sc.cb_off = (uint32_t)(numpage * page + sc.bank * page);
	if (sc.cb_off + 0x1000 > SC_SMMU_BYTES) {
		return ENODEV;
	}
	const uint32_t was_sctlr = sc_rd(sc.cb_off), was_actlr = sc_rd(sc.cb_off + 4);
	const uint32_t was_cbar = sc_rd((uint32_t)(page + 4 * sc.bank)), was_cba2r = sc_rd((uint32_t)(page + 0x800 + 4 * sc.bank));
	if (was_sctlr & 1) {
		printf("PANDORA SCANOUT: bank %u already translates (sctlr %08x): CPU copy\n", sc.bank, was_sctlr);
		return ENODEV;
	}

	/* Tables: the panel memory at its own address. */
	if (kmem_alloc_contig(kernel_map, &sc.tva, SC_TABLE_PAGES * 0x1000, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return ENOMEM;
	}
	sc.tpa = ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)sc.tva));
	static const uint8_t oas_bits[16] = {32, 36, 40, 42, 44, 48};
	const unsigned oas = MIN(((id2 >> 4) & 15) < 6 ? oas_bits[(id2 >> 4) & 15] : 32, 40);
	sc.tcfg = (ArmSmmu4KConfig){ 32, oas, sc.tpa, (size_t)((hi - lo) / 0x1000) };
	const ArmSmmu4KRange panel = { lo, lo, hi - lo, ArmSmmu4KRead, 0, 2 };
	ArmSmmu4KStatus st = ArmSmmu4KBuild(&sc.tcfg, &panel, 1, (uint64_t *)sc.tva, SC_TABLE_PAGES, &sc.tables);
	if (st != ArmSmmu4KSuccess) {
		printf("PANDORA SCANOUT: tables: status %d: CPU copy\n", st);
		return EINVAL;
	}
	sc.tnext = (unsigned)sc.tables.table_pages;
	clean_dcache64((addr64_t)sc.tpa, SC_TABLE_PAGES * 0x1000, TRUE);

	/* The bank: platform ACTLR, then ArmSmmu4K programs it, reads it back and turns translation on. */
	sc.mmio.mapping = (volatile unsigned char *)sc.smmu;
	sc.mmio.bytes = SC_SMMU_BYTES;
	if (ArmSmmu4KBindMmio(&sc.mmio, &sc.io) != ArmSmmu4KSuccess) {
		return EIO;
	}
	printf("PANDORA SCANOUT: stream match %u (S2CR %08x) -> bank %u: sctlr %08x actlr %08x cbar %08x cba2r %08x; "
	    "turning its translation on, %zu table pages\n", sc.smr, sc.panel_s2cr, sc.bank, was_sctlr, was_actlr, was_cbar,
	    was_cba2r, sc.tables.table_pages);
	(void)pandora_log_sync();
	const uint32_t f0 = dpu_rd(DPU_INTF(1) + 0xac);
	sc_wr(sc.cb_off + 4, SC_ACTLR);
	sc.ctx = (ArmSmmu4KContextConfig){ &sc.tcfg, NULL, &sc.tables, NULL, id0, id2, 1, 0, 0, 0 };
	ArmSmmu4KProgramResult prog;
	st = ArmSmmu4KProgramDisabledContext(&sc.io, sc.bank, &sc.ctx, 1000, &prog);
	IOSleep(100);
	const uint32_t fsr = sc_rd(sc.cb_off + 0x58), f1 = dpu_rd(DPU_INTF(1) + 0xac);
	printf("PANDORA SCANOUT: bank %u: status %d sctlr %08x tcr %08x ttbr0 %#llx; 100 ms later fsr %08x far %08x%08x, "
	    "frames %u -> %u\n", sc.bank, st, prog.readback.sctlr, prog.readback.tcr, prog.readback.ttbr0, fsr,
	    sc_rd(sc.cb_off + 0x64), sc_rd(sc.cb_off + 0x60), f0, f1);
	if (st != ArmSmmu4KSuccess || fsr != 0 || f1 == f0) {
		/* Back as the ABL left it: translation off, its attributes. */
		sc_wr(sc.cb_off, was_sctlr);
		sc_wr(sc.cb_off + 4, was_actlr);
		sc_wr((uint32_t)(page + 4 * sc.bank), was_cbar);
		sc_wr((uint32_t)(page + 0x800 + 4 * sc.bank), was_cba2r);
		if (fsr) {
			sc_wr(sc.cb_off + 0x58, fsr);
		}
		printf("PANDORA SCANOUT: bank %u back to translation off (sctlr %08x): CPU copy\n", sc.bank, sc_rd(sc.cb_off));
		return EIO;
	}
	for (unsigned p = 0; p < SC_PIPES; p++) {
		for (unsigned r = 0; r < SC_SAVED; r++) {
			sc.saved[p][r] = dpu_rd(sc_rec[p] + sc_save_regs[r]);
		}
		sc.saved_qop[p] = dpu_rd(sc_qseed[p] + SC_Q_OP_MODE);
	}
	sc.on_panel = true;
	sc.ready = true;
	printf("PANDORA SCANOUT: the display reads through bank %u's tables, no fault: frames can be scanned out\n", sc.bank);
	return 0;
}

/* A slot of 16 MiB of IOVA with PAGES mapped, or 0. */
uint32_t pandora_scanout_map(const uint64_t *pages, uint32_t npages);
uint32_t
pandora_scanout_map(const uint64_t *pages, uint32_t npages)
{
	if (!sc.ready || npages == 0 || npages > SC_SLOT_BYTES / 0x1000) {
		return 0;
	}
	for (unsigned i = 0; i < SC_SLOTS; i++) {
		if (!sc.slot_used[i]) {
			const uint32_t iova = SC_SLOT_BASE + i * SC_SLOT_BYTES;
			if (!sc_map(iova, pages, 0, npages)) {
				sc_unmap(iova, npages);
				return 0;
			}
			sc.slot_used[i] = true;
			return iova;
		}
	}
	return 0;
}

void pandora_scanout_unmap(uint32_t iova, uint32_t npages);
void
pandora_scanout_unmap(uint32_t iova, uint32_t npages)
{
	if (!sc.ready || iova < SC_SLOT_BASE || iova >= SC_SLOT_BASE + SC_SLOTS * SC_SLOT_BYTES) {
		return;
	}
	sc_unmap(iova, npages);
	sc.slot_used[(iova - SC_SLOT_BASE) / SC_SLOT_BYTES] = false;
}

/*
 * Show a W x H frame at ADDR (IOVA of its first pixel, rows of STRIDE bytes) on the main screen: scaled to the
 * whole panel with FILL, else centred at its own size. Returns false when the previous flush is still pending.
 */
bool pandora_scanout_show(uint32_t addr, uint32_t stride, uint32_t w, uint32_t h, bool fill);
bool
pandora_scanout_show(uint32_t addr, uint32_t stride, uint32_t w, uint32_t h, bool fill)
{
	if (!sc.ready || w < 2 || h == 0 || w > SC_PANEL_W || h > SC_PANEL_H || !sc_flush_idle()) {
		return false;
	}
	/* Both modes split at the mixers' boundary (panel x 610), as the ABL's setup does: scaled, the source splits
	 * at its middle (mainline splits a plane the same way); centred, at the column that lands on x 610. */
	const uint32_t left = fill ? 0 : (SC_PANEL_W - w) / 2;
	const uint32_t half = fill ? w / 2 : SC_HALF_W - left;
	for (unsigned p = 0; p < SC_PIPES; p++) {
		const uint32_t sx = p ? half : 0, sw = p ? w - half : half;
		uint32_t dx, dw, dy, dh;
		if (fill) {
			dx = p ? SC_HALF_W : 0;
			dw = SC_HALF_W;
			dy = 0;
			dh = SC_PANEL_H;
		} else {
			dx = left + sx;
			dw = sw;
			dy = (SC_PANEL_H - h) / 2;
			dh = h;
		}
		const uint32_t r = sc_rec[p], q = sc_qseed[p];
		dpu_wr(r + SC_SRC0_ADDR, addr);
		dpu_wr(r + SC_YSTRIDE0, stride & 0xffff);
		dpu_wr(r + SC_SRC_SIZE, (h << 16) | sw);
		dpu_wr(r + SC_SRC_XY, sx);
		dpu_wr(r + SC_OUT_SIZE, (dh << 16) | dw);
		dpu_wr(r + SC_OUT_XY, (dy << 16) | dx);
		if (dw != sw || dh != h) {
			const uint32_t sh = (uint32_t)(((uint64_t)sw << 21) / dw), sv = (uint32_t)(((uint64_t)h << 21) / dh);
			dpu_wr(r + SC_PIX_EXT_LR, 0);
			dpu_wr(r + SC_PIX_EXT_TB, 0);
			dpu_wr(r + SC_PIX_EXT_LR_ODX, 0);
			dpu_wr(r + SC_PIX_EXT_TB_ODX, 0);
			dpu_wr(r + SC_OP_MODE, sc.saved[p][0] | (1u << 31));          /* SW pixel extension override */
			dpu_wr(q + SC_Q_INIT_Y_H, 0);
			dpu_wr(q + SC_Q_INIT_Y_V, 0);
			dpu_wr(q + SC_Q_INIT_UV_H, 0);
			dpu_wr(q + SC_Q_INIT_UV_V, 0);
			dpu_wr(q + SC_Q_STEP_Y_H, sh & 0xffffff);
			dpu_wr(q + SC_Q_STEP_Y_V, sv & 0xffffff);
			dpu_wr(q + SC_Q_STEP_UV_H, sh & 0xffffff);
			dpu_wr(q + SC_Q_STEP_UV_V, sv & 0xffffff);
			dpu_wr(q + SC_Q_PRELOAD, 4u | (2u << 8) | (4u << 16) | (2u << 24));
			dpu_wr(q + SC_Q_SRC_SIZE, sw | (h << 16));
			dpu_wr(q + SC_Q_SRC_SIZE_UV, sw | (h << 16));
			dpu_wr(q + SC_Q_DST_SIZE, dw | (dh << 16));
			/* enable, Y/RGB filter bilinear (3), blend, non-DX format, alpha bilinear (1) */
			dpu_wr(q + SC_Q_OP_MODE, 1u | (3u << 16) | (1u << 31) | (1u << 14) | (1u << 10) | (1u << 29));
		} else {
			dpu_wr(r + SC_OP_MODE, sc.saved[p][0]);
			dpu_wr(q + SC_Q_OP_MODE, sc.saved_qop[p]);
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	dpu_wr(DPU_CTL(0) + 0x18, SC_FLUSH);
	sc.on_panel = false;
	sc.shows++;
	return true;
}

/* The ABL's own pipe setup again: the panel memory, unscaled. */
bool pandora_scanout_panel(void);
bool
pandora_scanout_panel(void)
{
	if (!sc.ready || sc.on_panel) {
		return true;
	}
	if (!sc_flush_idle()) {
		return false;
	}
	for (unsigned p = 0; p < SC_PIPES; p++) {
		for (unsigned r = 0; r < SC_SAVED; r++) {
			dpu_wr(sc_rec[p] + sc_save_regs[r], sc.saved[p][r]);
		}
		dpu_wr(sc_qseed[p] + SC_Q_OP_MODE, sc.saved_qop[p]);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	dpu_wr(DPU_CTL(0) + 0x18, SC_FLUSH);
	sc.on_panel = true;
	return true;
}

/* Whether both pipes now read ADDR (0: the panel memory); and the bank's fault status, cleared once read. */
bool pandora_scanout_current(uint32_t addr);
bool
pandora_scanout_current(uint32_t addr)
{
	if (!sc.ready) {
		return true;
	}
	const uint32_t want = addr ? addr : sc.panel_addr;
	const uint32_t fsr = sc_rd(sc.cb_off + 0x58);
	if (fsr) {
		sc.faults++;
		if (sc.faults <= 8) {
			printf("PANDORA SCANOUT: fault fsr %08x far %08x%08x fsynr0 %08x\n", fsr, sc_rd(sc.cb_off + 0x64),
			    sc_rd(sc.cb_off + 0x60), sc_rd(sc.cb_off + 0x68));
		}
		sc_wr(sc.cb_off + 0x58, fsr);
	}
	return dpu_rd(sc_rec[0] + SC_CURRENT0) == want && dpu_rd(sc_rec[1] + SC_CURRENT0) == want;
}

bool pandora_scanout_ready(void);
bool
pandora_scanout_ready(void)
{
	return sc.ready;
}

void pandora_scanout_counts(uint64_t *shows, uint64_t *faults);
void
pandora_scanout_counts(uint64_t *shows, uint64_t *faults)
{
	*shows = sc.shows;
	*faults = sc.faults;
}
#endif /* PANDORA */

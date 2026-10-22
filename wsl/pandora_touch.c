/*
 * PANDORA (Xiaomi 17 Pro, SM8850) main screen touch: Synaptics S3910P speaking
 * TouchComm (v1 or v2, detected) over SPI on QUPv3 wrapper 4 SE0 (0x01a80000),
 * polled through GPI DMA. Hardware from the live DT (device/DT_FINDINGS.md):
 *   IRQ    TLMM GPIO 158, active low        reset  GPIO 66, active low
 *   avdd   fixed regulator, GPIO 117 high   iovdd  RPMh LDO "L4I_E0" 1.8 V (pmh0110_i_l4)
 *   SPI    GPIO 48 MISO, 49 MOSI, 50 CLK, 51 CS, function 1 (qup4_se0), 6 mA, pull-down
 * TLMM pins are 0x1000 apart from 0x0f100000: the downstream pinctrl-canoe.ko adds
 * 0x100000 to the DT base 0x0f000000 (its pin table holds 0x100000 + 0x1000 * n), and
 * 0x0f0xxxxx hangs the bus. GCC offsets from gcc-kaanapali.c (wrapper 4: vote 0x52018
 * bits 22-26, SE0 RCG 0xa9014); GENI and SPI registers as Linux qcom-geni-se.c and
 * spi-geni-qcom.c; TouchComm as Synaptics' syna_c10 driver (tcm/*.c).
 *
 * Boot-arg pandora_touch (bit mask, default 0 = off):
 *   bit 0  probe: print GCC, SE and pin state (votes the wrapper 4 clocks on)
 *   bit 1  vote L4I_E0 on (1.8 V, high power mode) through RPMh, apps RSC DRV 2
 *   bit 2  run: SPI set-up, avdd, reset, TouchComm identify, application info, touch
 *          report config, then report touches to SweetHIDTouch
 * pandora_steps=MS: before the first access to each new register region print its
 * name and wait MS (default 0), so a bus hang leaves the name in the log file.
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

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern kern_return_t thread_terminate(thread_t);
extern void sweet_hid_touch(int finger, int down, int x, int y);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation,
    void *parameter, integer_t priority, thread_t *new_thread);
extern int sweet_fb_display_off;       /* iokit/Kernel/arm/SweetFramebuffer.cpp */
void pandora_touch_init(void);

#define PT_GCC_PA     0x00100000u
#define PT_SE_PA      0x01a80000u
#define PT_TLMM_PA    0x0f100000u
#define PT_RSC_PA     0x18920000u      /* apps RSC "drv-2" */
#define PT_CMDDB_PA   0x81c60000u      /* reserved-memory aop_cmd_db_region */
#define PT_CMDDB_SIZE 0x20000u
#define PT_GPIO(n)    ((uint32_t)(n) * 0x1000u)
#define PT_IRQ        158u
#define PT_RESET      66u
#define PT_AVDD       117u
#define PT_MAX        10u
#define PT_PRIORITY   92               /* BASEPRI_PREEMPT, as fuxi_touch */

/* GCC */
#define GCC_VOTE      0x52018u
#define GCC_VOTE_BITS (0x1fu << 22)    /* m_ahb, s_ahb, core, core2x, s0 */
#define GCC_M_AHB     0x23548u
#define GCC_S_AHB     0x2354cu
#define GCC_CORE      0x23550u
#define GCC_CORE2X    0x23564u
#define GCC_S0        0xa9004u
#define GCC_S0_RCG    0xa9014u         /* CMD; CFG +4, DFSR +0x14, PERF_DFSR(l) +0x1c+4l */

/* GENI serial engine */
#define SE_FORCE_DEFAULT 0x020u
#define SE_OUTPUT_CTRL   0x024u
#define SE_CGC_CTRL      0x028u
#define SE_STATUS        0x040u
#define SE_M_CLK_CFG     0x048u
#define SE_IF_DISABLE    0x064u
#define SE_FW_REV        0x068u
#define SE_CLK_SEL       0x07cu
#define SE_SPI_CPHA      0x224u
#define SE_SPI_LOOPBACK  0x22cu
#define SE_SPI_CPOL      0x230u
#define SE_SPI_DEMUX_INV 0x24cu
#define SE_SPI_DEMUX_SEL 0x250u
#define SE_BYTE_GRAN     0x254u
#define SE_DMA_MODE      0x258u
#define SE_TX_PACK0      0x260u
#define SE_TX_PACK1      0x264u
#define SE_SPI_WORD_LEN  0x268u
#define SE_SPI_TX_LEN    0x26cu
#define SE_SPI_RX_LEN    0x270u
#define SE_RX_PACK0      0x284u
#define SE_RX_PACK1      0x288u
#define SE_M_CMD0        0x600u
#define SE_M_CMD_CTRL    0x604u
#define SE_M_IRQ         0x610u
#define SE_M_IRQ_EN      0x614u
#define SE_M_IRQ_CLR     0x618u
#define SE_S_IRQ_CLR     0x648u
#define SE_TX_FIFO       0x700u
#define SE_RX_FIFO       0x780u
#define SE_TX_STATUS     0x800u
#define SE_RX_STATUS     0x804u
#define SE_TX_WM         0x80cu
#define SE_RX_WM         0x810u
#define SE_RX_RFR_WM     0x814u
#define SE_DMA_TX_CLR    0xc44u
#define SE_DMA_RX_CLR    0xd44u
#define SE_GSI_EVENT_EN  0xe18u
#define SE_IRQ_EN        0xe1cu
#define SE_HW_PARAM0     0xe24u
#define SE_HW_PARAM1     0xe28u
#define PT_SPI_DIV       4u            /* 19.2 MHz XO / 4 = 4.8 MHz (IC allows 15 MHz) */
#define PT_XFER_MAX      2048u

/* TouchComm */
#define TC_MARKER        0xa5u
#define TC_PAD           0x5au
#define TC_CONTINUED     0x03u
#define TC_OK            0x01u
#define TC_NO_REPORT     0x04u
#define TC_ACK           0x07u
#define TC_IDENTIFY      0x10u
#define TC_TOUCH         0x11u
#define CMD_IDENTIFY     0x02u
#define CMD_ENABLE_REPORT 0x05u
#define CMD_V2_ACK       0x07u
#define CMD_V2_RETRY     0x08u
#define CMD_V2_GET_REPORT 0x0au
#define CMD_APP_INFO     0x20u
#define CMD_GET_TOUCH_CONFIG 0x25u
#define PT_MSG_MAX       1024u

static struct {
	vm_offset_t gcc, se, tlmm;
	bool started, dead;
	unsigned steps_ms, level, version;     /* version: TouchComm 1 or 2 */
	bool crc;                              /* v1: CRC-16 after the payload */
	unsigned seq;                          /* v2: header sequence bit */
	unsigned max_rd, max_wr;
	unsigned max_x, max_y, max_objects;
	bool fill;
	uint8_t msg[PT_MSG_MAX];
	unsigned msg_len;
	uint8_t config[256];
	unsigned config_len;
	uint32_t active;
	int x[PT_MAX], y[PT_MAX];
	uint32_t codes_seen[8];                /* bitmap of report codes already logged */
	unsigned reports, touch_reports;
} pt;
static uint8_t pt_buf[PT_XFER_MAX + 8];
static int pt_chan;

static uint32_t
pt_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
pt_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
pt_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&pt_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
pt_sleep_until(uint64_t deadline)
{
	(void)assert_wait_deadline_with_leeway(&pt_chan, THREAD_UNINT,
	    TIMEOUT_URGENCY_SYS_CRITICAL, deadline, 0);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
pt_step(const char *what, uint32_t pa)
{
	if (pt.steps_ms) {
		printf("PANDORA TOUCH: next: first access to %s at %#x\n", what, pa);
		pt_sleep(pt.steps_ms);
	}
}

static bool
pt_wait_clear(vm_offset_t base, uint32_t off, uint32_t mask, unsigned us)
{
	for (unsigned i = 0; i < us; i++) {
		if (!(pt_rd(base, off) & mask)) {
			return true;
		}
		delay(1);
	}
	return false;
}

/* ---- pins ---------------------------------------------------------------------- */

/* TLMM CTL: pull [1:0] (1 down, 3 up), func [5:2], drive [8:6] = mA/2-1, OE [9],
 * EGPIO_PRESENT [11], EGPIO_ENABLE [12] (set: the pin stays with the TLMM). */
static void
pt_gpio(unsigned n, unsigned func, unsigned pull, unsigned ma, int out)
{
	if (out >= 0) {
		pt_wr(pt.tlmm, PT_GPIO(n) + 4, out ? 2u : 0u);
	}
	uint32_t ctl = pt_rd(pt.tlmm, PT_GPIO(n)) & ~0x3ffu;
	ctl |= pull | (func << 2) | ((ma / 2 - 1) << 6) | (out >= 0 ? 0x200u : 0u);
	if (ctl & (1u << 11)) {
		ctl |= 1u << 12;
	}
	pt_wr(pt.tlmm, PT_GPIO(n), ctl);
}

static void
pt_pins_dump(const char *when)
{
	static const uint8_t pins[] = {48, 49, 50, 51, PT_RESET, PT_AVDD, PT_IRQ};
	printf("PANDORA TOUCH: pins %s:", when);
	for (unsigned i = 0; i < sizeof(pins); i++) {
		printf(" %u=%#x/%u", pins[i], pt_rd(pt.tlmm, PT_GPIO(pins[i])),
		    pt_rd(pt.tlmm, PT_GPIO(pins[i]) + 4) & 3u);
	}
	printf("\n");
}

/* ---- clocks and serial engine -------------------------------------------------- */

static bool
pt_on(uint32_t cbcr)
{
	return !(pt_rd(pt.gcc, cbcr) & 0x80000000u);
}

static void
pt_clock_dump(const char *when)
{
	uint32_t r = GCC_S0_RCG;
	printf("PANDORA TOUCH: gcc %s: vote=%#x m_ahb=%#x s_ahb=%#x core=%#x core2x=%#x s0=%#x "
	    "rcg=%#x cfg=%#x dfs=%#x perf0..3=%#x %#x %#x %#x\n", when,
	    pt_rd(pt.gcc, GCC_VOTE), pt_rd(pt.gcc, GCC_M_AHB), pt_rd(pt.gcc, GCC_S_AHB),
	    pt_rd(pt.gcc, GCC_CORE), pt_rd(pt.gcc, GCC_CORE2X), pt_rd(pt.gcc, GCC_S0),
	    pt_rd(pt.gcc, r), pt_rd(pt.gcc, r + 4), pt_rd(pt.gcc, r + 0x14),
	    pt_rd(pt.gcc, r + 0x1c), pt_rd(pt.gcc, r + 0x20), pt_rd(pt.gcc, r + 0x24),
	    pt_rd(pt.gcc, r + 0x28));
}

static int
pt_clocks(void)
{
	pt_wr(pt.gcc, GCC_VOTE, pt_rd(pt.gcc, GCC_VOTE) | GCC_VOTE_BITS);
	static const uint32_t branches[] = {GCC_M_AHB, GCC_S_AHB, GCC_CORE, GCC_CORE2X};
	for (unsigned i = 0; i < 4; i++) {
		for (unsigned t = 0; t < 10000 && !pt_on(branches[i]); t++) {
			delay(1);
		}
		if (!pt_on(branches[i])) {
			pt_clock_dump("branch off");
			return EIO;
		}
	}
	/* SE0 source: XO undivided (19.2 MHz). With DFS the SE picks a performance level
	 * (SE_CLK_SEL); without, set the RCG to XO/1 (no PLL or voltage change). */
	uint32_t r = GCC_S0_RCG;
	unsigned level = 0;
	if (pt_rd(pt.gcc, r + 0x14) & 1u) {
		while (level < 8 && (pt_rd(pt.gcc, r + 0x1c + 4 * level) & 0x371fu) != 1u) {
			level++;
		}
		if (level == 8) {
			pt_clock_dump("no XO DFS level");
			return EIO;
		}
	} else {
		uint32_t cfg = (pt_rd(pt.gcc, r + 4) & ~(0x371fu | (1u << 20))) | 1u;
		pt_wr(pt.gcc, r + 4, cfg);
		pt_wr(pt.gcc, r, pt_rd(pt.gcc, r) | 1u);
		if (!pt_wait_clear(pt.gcc, r, 1u, 5000)) {
			pt_clock_dump("RCG update timeout");
			return EIO;
		}
	}
	pt.level = level;
	return 0;
}

static void
pt_se_dump(const char *when)
{
	printf("PANDORA TOUCH: se %s: fw=%#x if_disable=%#x status=%#x hw0=%#x hw1=%#x "
	    "clk_sel=%#x m_clk=%#x dma=%#x gsi_ev=%#x irq_en=%#x m_irq=%#x m_irq_en=%#x "
	    "word=%#x tx=%#x rx=%#x\n", when,
	    pt_rd(pt.se, SE_FW_REV), pt_rd(pt.se, SE_IF_DISABLE), pt_rd(pt.se, SE_STATUS),
	    pt_rd(pt.se, SE_HW_PARAM0), pt_rd(pt.se, SE_HW_PARAM1), pt_rd(pt.se, SE_CLK_SEL),
	    pt_rd(pt.se, SE_M_CLK_CFG), pt_rd(pt.se, SE_DMA_MODE), pt_rd(pt.se, SE_GSI_EVENT_EN),
	    pt_rd(pt.se, SE_IRQ_EN), pt_rd(pt.se, SE_M_IRQ), pt_rd(pt.se, SE_M_IRQ_EN),
	    pt_rd(pt.se, SE_SPI_WORD_LEN), pt_rd(pt.se, SE_TX_STATUS), pt_rd(pt.se, SE_RX_STATUS));
}

/*
 * SE0's firmware has the FIFO interface disabled (GENI_IF_DISABLE_RO bit 0): transfers go
 * through the wrapper 4 GPI DMA engine (qcom,gpi-dma@1a00000, ee-offset 0x10000), as
 * fuxi_touch.c does on the Xiaomi 13. GPII 1 (first of the DT's HLOS gpii-mask 0x1e):
 * TX channel 0, RX channel 1, one event ring. Per-GPII window = ee base (0x1a00000 -
 * 0x10000) + 0x20000 + 0x4000 * n: channel contexts +0, event context +0x1000, channel
 * doorbells +0x2000, event doorbell +0x2100, GPII registers +0x3000 (Linux qcom gpi.c).
 * The GPI (stream 0x536) and QUP (0x523) need apps SMMU stream matches: each goes to the
 * context bank that UFS's stream 0x60 already uses (stage 1 off, sweet_ufs.c).
 */
#define PT_GPII       1u
#define PT_GPI_PA     (0x01a00000u - 0x10000u + 0x20000u + 0x4000u * PT_GPII)
#define PT_SMMU_PA    0x15000000u
#define PT_DMA_SIZE   0x4000u
#define FT_EV_OFF     0x0000u
#define FT_RING_OFF(k) (0x1000u + 0x400u * (k))
#define FT_TXB_OFF    0x2000u          /* up to 0x800 */
#define FT_RXB_OFF    0x3000u          /* up to 0x1000 */
#define FT_RING       64u
#define FT_EVRING     256u
#define GPI_CH(k)     (0x0000u + 0x80u * (k))
#define GPI_EV        0x1000u
#define GPI_CHDB(k)   (0x2000u + 8u * (k))
#define GPI_EVDB      0x2100u
#define GPI_GPII      0x3000u

static vm_offset_t pt_gpi, pt_smmu, pt_dma;
static uint64_t pt_dma_pa;
static unsigned pt_wp[2], pt_evrp;

static void
pt_dma_sync(uint32_t off, uint32_t len)
{
	flush_dcache64((addr64_t)(pt_dma_pa + off), len, TRUE);
}

static uint32_t
pt_gstate(uint32_t ctx)
{
	return (pt_rd(pt_gpi, ctx) >> 20) & 0xf;
}

static bool
pt_gpi_cmd(uint32_t reg, uint32_t v, uint32_t ctx, uint32_t want)
{
	pt_wr(pt_gpi, GPI_GPII + reg, v);
	for (unsigned i = 0; i < 50000; i++) {
		uint32_t st = pt_gstate(ctx);
		if (st == want) {
			pt_wr(pt_gpi, GPI_GPII + 0xa0, pt_rd(pt_gpi, GPI_GPII + 0x90));
			pt_wr(pt_gpi, GPI_GPII + 0xa4, pt_rd(pt_gpi, GPI_GPII + 0x94));
			return true;
		}
		if (st == 0xf) {
			break;
		}
		delay(1);
	}
	printf("PANDORA TOUCH: GPI cmd %#x -> state %#x (want %u), error log %#x\n",
	    v, pt_gstate(ctx), want, pt_rd(pt_gpi, GPI_GPII + 0x200));
	return false;
}

/* Stream matches for the GPI and the QUP, pointing where UFS's 0x60 points. Idempotent. */
static int
pt_smmu_setup(void)
{
	static const uint16_t sid[] = {0x536, 0x523};
	uint32_t nsmr = pt_rd(pt_smmu, 0x20) & 0xff, s2cr = 0;
	bool found = false;
	for (unsigned i = 0; i < nsmr; i++) {
		uint32_t smr = pt_rd(pt_smmu, 0x800 + 4 * i);
		if ((smr & 0x80000000u) && ((0x60u ^ smr) & ~(smr >> 16) & 0x7fff) == 0) {
			s2cr = pt_rd(pt_smmu, 0xc00 + 4 * i);
			found = true;
			break;
		}
	}
	if (!found || ((s2cr >> 16) & 3) != 0) {
		printf("PANDORA TOUCH: no UFS stream match to copy (s2cr %#x)\n", s2cr);
		return ENODEV;
	}
	for (unsigned j = 0; j < 2; j++) {
		unsigned i, freei = ~0u;
		for (i = 0; i < nsmr; i++) {
			uint32_t smr = pt_rd(pt_smmu, 0x800 + 4 * i);
			if (!(smr & 0x80000000u)) {
				if (freei == ~0u) {
					freei = i;
				}
				continue;
			}
			if (((sid[j] ^ smr) & ~(smr >> 16) & 0x7fff) == 0) {
				break;
			}
		}
		if (i < nsmr) {
			printf("PANDORA TOUCH: sid %#x already matched by SMR %u (S2CR %#x)\n", sid[j], i,
			    pt_rd(pt_smmu, 0xc00 + 4 * i));
			continue;
		}
		if (freei == ~0u) {
			return ENOSPC;
		}
		pt_wr(pt_smmu, 0xc00 + 4 * freei, s2cr);
		pt_wr(pt_smmu, 0x800 + 4 * freei, 0x80000000u | sid[j]);
		printf("PANDORA TOUCH: SMR %u = sid %#x -> S2CR %#x (as UFS)\n", freei, sid[j],
		    pt_rd(pt_smmu, 0xc00 + 4 * freei));
	}
	return 0;
}

static int
pt_gpi_init(void)
{
	uint32_t lo = (uint32_t)pt_dma_pa, hi = (uint32_t)(pt_dma_pa >> 32);
	for (unsigned k = 0; k < 2; k++) {
		if (pt_gstate(GPI_CH(k)) == 2) {
			(void)pt_gpi_cmd(0x08, (2u << 24) | k, GPI_CH(k), 3);
		}
		if (pt_gstate(GPI_CH(k))) {
			(void)pt_gpi_cmd(0x08, (9u << 24) | k, GPI_CH(k), 1);
			pt_wr(pt_gpi, GPI_GPII + 0x08, (10u << 24) | k);
			delay(1000);
		}
	}
	if (pt_gstate(GPI_EV)) {
		(void)pt_gpi_cmd(0x10, 9u << 24, GPI_EV, 1);
		pt_wr(pt_gpi, GPI_GPII + 0x10, 10u << 24);
		delay(1000);
	}
	bzero((void *)pt_dma, PT_DMA_SIZE);
	pt_dma_sync(0, PT_DMA_SIZE);
	pt_wr(pt_gpi, GPI_GPII + 0x88, 0x4f);
	pt_wr(pt_gpi, GPI_GPII + 0xb8, 1);
	pt_wr(pt_gpi, GPI_GPII + 0x98, 3);
	pt_wr(pt_gpi, GPI_GPII + 0x9c, 1);
	pt_wr(pt_gpi, GPI_GPII + 0x108, 1);
	pt_wr(pt_gpi, GPI_GPII + 0x120, 0xf);
	pt_wr(pt_gpi, GPI_GPII + 0x188, 0);
	pt_wr(pt_gpi, GPI_GPII + 0x18c, 0);
	pt_wr(pt_gpi, GPI_GPII + 0x400, 0);
	pt_wr(pt_gpi, GPI_GPII + 0x404, 0);
	pt_wr(pt_gpi, GPI_GPII + 0x180, 1);
	pt_wr(pt_gpi, GPI_GPII + 0x200, 0);
	if (!pt_gpi_cmd(0x10, 0, GPI_EV, 1)) {
		return EIO;
	}
	pt_wr(pt_gpi, GPI_EV + 0x00, 0x10010002u);
	pt_wr(pt_gpi, GPI_EV + 0x04, FT_EVRING * 16);
	pt_wr(pt_gpi, GPI_EV + 0x08, lo + FT_EV_OFF);
	pt_wr(pt_gpi, GPI_EV + 0x0c, hi);
	pt_wr(pt_gpi, GPI_EVDB + 4, hi);
	for (uint32_t o = 0x20; o <= 0x34; o += 4) {
		pt_wr(pt_gpi, GPI_EV + o, 0);
	}
	pt_evrp = 0;
	pt_wr(pt_gpi, GPI_EVDB, lo + FT_EV_OFF + FT_EVRING * 16 - 16);
	for (unsigned k = 0; k < 2; k++) {
		if (!pt_gpi_cmd(0x08, k, GPI_CH(k), 1)) {
			return EIO;
		}
		pt_wr(pt_gpi, GPI_CH(k) + 0x00, k ? 0x10000002u : 0x1000000au);
		pt_wr(pt_gpi, GPI_CH(k) + 0x04, FT_RING * 16);
		pt_wr(pt_gpi, GPI_CH(k) + 0x08, lo + FT_RING_OFF(k));
		pt_wr(pt_gpi, GPI_CH(k) + 0x0c, hi);
		pt_wr(pt_gpi, GPI_CHDB(k) + 4, hi);
		/* SCRATCH_0: pair channel, protocol 1 (SPI), SE 0. */
		pt_wr(pt_gpi, GPI_CH(k) + 0x60, k ? 0x00000010u : 0x00010010u);
		pt_wr(pt_gpi, GPI_CH(k) + 0x64, 0);
		pt_wr(pt_gpi, GPI_CH(k) + 0x68, 0);
		pt_wr(pt_gpi, GPI_CH(k) + 0x6c, 0);
		pt_wr(pt_gpi, GPI_CH(k) + 0x5c, 1);
		pt_wp[k] = 0;
	}
	for (unsigned k = 0; k < 2; k++) {
		if (!pt_gpi_cmd(0x08, (1u << 24) | k, GPI_CH(k), 2)) {
			return EIO;
		}
	}
	return 0;
}

/* geni_se_select_mode(GENI_GPI_DMA); the CONFIG0 TRE sets word size, packing and clock. */
static int
pt_se_init(void)
{
	uint32_t fw = pt_rd(pt.se, SE_FW_REV);
	if (((fw >> 8) & 0xff) != 1) {
		printf("PANDORA TOUCH: SE0 firmware %#x is not SPI\n", fw);
		return ENODEV;
	}
	if (pt_rd(pt.se, SE_STATUS) & 1u) {
		printf("PANDORA TOUCH: SE0 busy (status %#x)\n", pt_rd(pt.se, SE_STATUS));
		return EBUSY;
	}
	pt_wr(pt.se, SE_GSI_EVENT_EN, 0);
	pt_wr(pt.se, SE_M_IRQ_CLR, 0xffffffffu);
	pt_wr(pt.se, SE_S_IRQ_CLR, 0xffffffffu);
	pt_wr(pt.se, SE_DMA_TX_CLR, 0xffffffffu);
	pt_wr(pt.se, SE_DMA_RX_CLR, 0xffffffffu);
	pt_wr(pt.se, SE_CGC_CTRL, pt_rd(pt.se, SE_CGC_CTRL) | 0x7fu);
	pt_wr(pt.se, 0xe30, pt_rd(pt.se, 0xe30) | 0xfu);
	pt_wr(pt.se, SE_OUTPUT_CTRL, 0x7fu);
	pt_wr(pt.se, SE_FORCE_DEFAULT, 1);
	pt_wr(pt.se, SE_IRQ_EN, 0);
	pt_wr(pt.se, SE_M_IRQ_EN, pt_rd(pt.se, SE_M_IRQ_EN) &
	    ~((1u << 0) | (1u << 26) | (1u << 27) | (1u << 30)));
	pt_wr(pt.se, 0x644, pt_rd(pt.se, 0x644) & ~1u);
	pt_wr(pt.se, SE_DMA_MODE, 1);
	pt_wr(pt.se, SE_GSI_EVENT_EN, pt_rd(pt.se, SE_GSI_EVENT_EN) | 0xfu);
	pt_step("apps SMMU", PT_SMMU_PA);
	pt_smmu = ml_io_map(PT_SMMU_PA, 0x1000);
	pt_gpi = ml_io_map(PT_GPI_PA, 0x4000);
	if (!pt_smmu || !pt_gpi) {
		return ENOMEM;
	}
	int err;
	if ((err = pt_smmu_setup())) {
		return err;
	}
	if (kmem_alloc_contig(kernel_map, &pt_dma, PT_DMA_SIZE, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("PANDORA TOUCH: no DMA memory\n");
		return ENOMEM;
	}
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)pt_dma);
	if (!pn) {
		return ENOMEM;
	}
	pt_dma_pa = ptoa_64(pn);
	if (pt_dma_pa + PT_DMA_SIZE > (1ULL << 36)) {
		printf("PANDORA TOUCH: DMA at %#llx is outside the SMMU's 36-bit range\n", pt_dma_pa);
		return ENOMEM;
	}
	pt_step("GPII 1 registers", PT_GPI_PA);
	printf("PANDORA TOUCH: GPII %u before: ch0 %#x ch1 %#x ev %#x\n", PT_GPII,
	    pt_rd(pt_gpi, GPI_CH(0)), pt_rd(pt_gpi, GPI_CH(1)), pt_rd(pt_gpi, GPI_EV));
	if ((err = pt_gpi_init())) {
		return err;
	}
	printf("PANDORA TOUCH: SE0 SPI over GPII %u, DMA page %#llx, DFS level %u, %u kHz\n",
	    PT_GPII, pt_dma_pa, pt.level, 19200u / PT_SPI_DIV);
	return 0;
}

static void
pt_tre(unsigned k, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
	uint32_t off = FT_RING_OFF(k) + 16 * pt_wp[k];
	volatile uint32_t *t = (volatile uint32_t *)(pt_dma + off);
	t[0] = d0;
	t[1] = d1;
	t[2] = d2;
	t[3] = d3;
	pt_dma_sync(off, 16);
	pt_wp[k] = (pt_wp[k] + 1) % FT_RING;
}

static void
pt_ring_db(unsigned k)
{
	__asm__ volatile("dsb sy" ::: "memory");
	pt_wr(pt_gpi, GPI_CHDB(k), (uint32_t)pt_dma_pa + FT_RING_OFF(k) + 16 * pt_wp[k]);
}

/* Drain events until end-of-transfer (code 2) has come from every channel in want. */
static int
pt_gpi_wait(unsigned want)
{
	unsigned seen = 0;
	for (unsigned us = 0; us < 50000; us++) {
		uint32_t hw = pt_rd(pt_gpi, GPI_EV + 0x10);
		bool bad = false;
		while (hw != (uint32_t)pt_dma_pa + FT_EV_OFF + 16 * pt_evrp) {
			uint32_t off = FT_EV_OFF + 16 * pt_evrp;
			pt_dma_sync(off, 16);
			volatile uint32_t *e = (volatile uint32_t *)(pt_dma + off);
			uint32_t d2 = e[2], d3 = e[3];
			pt_evrp = (pt_evrp + 1) % FT_EVRING;
			unsigned code = d2 >> 24, type = (d3 >> 16) & 0xff, ch = d3 >> 24;
			if (code == 16 || type == 0xff || ch >= 2) {
				bad = true;
				printf("PANDORA TOUCH: GPI event %#x %#x\n", d2, d3);
			} else if (type == 0x22 && code == 2) {
				seen |= 1u << ch;
			}
		}
		pt_wr(pt_gpi, GPI_EVDB, (uint32_t)pt_dma_pa + FT_EV_OFF +
		    16 * ((pt_evrp + FT_EVRING - 1) % FT_EVRING));
		pt_wr(pt_gpi, GPI_GPII + 0xc0, 1);
		if (bad) {
			return EIO;
		}
		if ((seen & want) == want) {
			return 0;
		}
		delay(1);
	}
	return ETIMEDOUT;
}

/* One full-duplex transfer with chip select held for its length. tx NULL sends 0xff
 * (TouchComm reads), rx NULL drops what comes back. */
static int
pt_spi(const uint8_t *tx, uint8_t *rx, unsigned n)
{
	if (pt.dead || n == 0 || n > PT_XFER_MAX) {
		return EIO;
	}
	uint8_t *txb = (uint8_t *)(pt_dma + FT_TXB_OFF), *rxb = (uint8_t *)(pt_dma + FT_RXB_OFF);
	if (tx) {
		memcpy(txb, tx, n);
	} else {
		memset(txb, 0xff, n);
	}
	pt_dma_sync(FT_TXB_OFF, n);
	pt_dma_sync(FT_RXB_OFF, n);
	uint32_t hi = (uint32_t)(pt_dma_pa >> 32);
	pt_tre(1, (uint32_t)pt_dma_pa + FT_RXB_OFF, hi, n, 0x00100200u);
	/* CONFIG0: 8-bit words (8 - 4), TX/RX packing, mode 0; clock level and divider. */
	pt_tre(0, 0x03000004u, 0, (pt.level << 16) | PT_SPI_DIV, 0x00220001u);
	/* GO: full duplex (3), RX length; chain + link. */
	pt_tre(0, 3u, 0, n, 0x00200801u);
	pt_tre(0, (uint32_t)pt_dma_pa + FT_TXB_OFF, hi, n, 0x00100200u);
	pt_ring_db(1);
	pt_ring_db(0);
	int err = pt_gpi_wait(3u);
	if (err) {
		static unsigned fails;
		if (fails++ < 10) {
			printf("PANDORA TOUCH: SPI over GPI failed %d (n %u), ch0 %#x ch1 %#x err log %#x\n",
			    err, n, pt_rd(pt_gpi, GPI_CH(0)), pt_rd(pt_gpi, GPI_CH(1)),
			    pt_rd(pt_gpi, GPI_GPII + 0x200));
		}
		if (pt_gpi_init()) {
			pt.dead = true;
		}
		return err;
	}
	pt_dma_sync(FT_RXB_OFF, n);
	if (rx) {
		memcpy(rx, rxb, n);
	}
	return 0;
}

/* ---- RPMh: L4I_E0 on ----------------------------------------------------------- */

/* Register layout per RSC major version (Linux rpmh-rsc.c ver_2_7 / ver_3_0). */
struct rh_layout {
	uint32_t wait, control, status, enable, msgid, addr, data, cmd_status, cmd_stride;
};
static const struct rh_layout rh_v2 = {0x10, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38, 0x3c, 20};
static const struct rh_layout rh_v3 = {0x20, 0x24, 0x28, 0x2c, 0x34, 0x38, 0x3c, 0x40, 24};
#define RH_TCS_OFF     0xd00u          /* DT drv@2 qcom,tcs-offset */
#define RH_TCS_STRIDE  0x2a0u          /* qcom,tcs-distance */
#define RH_ACTIVE_N    3u              /* qcom,tcs-config: ACTIVE 3 first */
#define RH_DRV         2u
#define RH_WRITE_ACK   (8u | (1u << 8) | (1u << 16))
#define RH_DB_MAGIC    0x0c0330dbu
#define RH_DB_HEADER   144u
#define RH_DB_ENTRY    24u

static vm_offset_t rh_drv, rh_db;

static uint16_t
rh_db16(uint32_t off)
{
	return (uint16_t)(pt_rd(rh_db, off & ~3u) >> (8u * (off & 3u)));
}

static int
rh_lookup(const char *res, uint32_t *address)
{
	uint32_t key[2] = {0, 0};
	for (unsigned i = 0; i < 8 && res[i]; i++) {
		key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8u * (i % 4));
	}
	unsigned matches = 0;
	for (unsigned i = 0; i < 8; i++) {
		uint32_t h = 8u + 16u * i;
		unsigned slave = rh_db16(h);
		if (!slave) {
			break;
		}
		uint32_t entries = RH_DB_HEADER + rh_db16(h + 2);
		unsigned count = rh_db16(h + 6);
		if ((entries & 3u) || entries + count * RH_DB_ENTRY > PT_CMDDB_SIZE) {
			printf("PANDORA TOUCH: cmd-db slave header %u malformed\n", i);
			return EINVAL;
		}
		for (unsigned j = 0; j < count; j++) {
			uint32_t e = entries + RH_DB_ENTRY * j;
			if (pt_rd(rh_db, e) != key[0] || pt_rd(rh_db, e + 4) != key[1]) {
				continue;
			}
			uint32_t addr = pt_rd(rh_db, e + 16);
			printf("PANDORA TOUCH: cmd-db %s addr=%#x slave=%u\n", res, addr, slave);
			if (slave == 4 && ((addr >> 16) & 7u) == 4 && !(addr & 15u)) {
				*address = addr;
				matches++;
			}
		}
	}
	if (matches != 1) {
		printf("PANDORA TOUCH: cmd-db %s: %u VRM matches; not voting\n", res, matches);
		return ENODEV;
	}
	return 0;
}

static bool
rh_sync(uint32_t off, uint32_t v)
{
	pt_wr(rh_drv, off, v);
	for (unsigned i = 0; i < 1000; i++) {
		if (pt_rd(rh_drv, off) == v) {
			return true;
		}
		delay(1);
	}
	printf("PANDORA TOUCH: RSC readback %#x want %#x got %#x\n", off, v, pt_rd(rh_drv, off));
	return false;
}

/* One ACTIVE TCS with three commands (voltage, mode, enable), as sweet_rpmh.c. */
static int
pt_rpmh_vote(void)
{
	pt_step("apps RSC DRV 2", PT_RSC_PA);
	rh_drv = ml_io_map(PT_RSC_PA, 0x10000);
	rh_db = ml_io_map(PT_CMDDB_PA, PT_CMDDB_SIZE);
	if (!rh_drv || !rh_db) {
		return ENOMEM;
	}
	uint32_t id = pt_rd(rh_drv, 0), config = pt_rd(rh_drv, 0x0c);
	unsigned major = (id >> 16) & 0xffu, minor = (id >> 8) & 0xffu;
	unsigned ntcs = (config >> (6u * RH_DRV)) & 0x3fu, ncpt = (config >> 27) & 0x1fu;
	printf("PANDORA TOUCH: RSC id=%#x (v%u.%u) solver=%#x config=%#x tcs=%u ncpt=%u\n",
	    id, major, minor, pt_rd(rh_drv, 4), config, ntcs, ncpt);
	/* Linux uses the 3.0 layout for every major >= 3 (this RSC reads v4.3). */
	const struct rh_layout *L = major >= 3 ? &rh_v3 : (major == 2 ? &rh_v2 : NULL);
	if (!L || ntcs < RH_ACTIVE_N || ntcs > 16 || ncpt < 3 || ncpt > 16) {
		printf("PANDORA TOUCH: RSC layout not known; not voting\n");
		return ENODEV;
	}
	pt_step("cmd-db", PT_CMDDB_PA);
	uint32_t magic = pt_rd(rh_db, 4);
	printf("PANDORA TOUCH: cmd-db version=%#x magic=%#x\n", pt_rd(rh_db, 0), magic);
	uint32_t address = 0;
	if (magic != RH_DB_MAGIC || rh_lookup("L4I_E0", &address)) {
		return ENODEV;
	}
	uint32_t tcs = RH_TCS_OFF;
	int chosen = -1;
	for (unsigned t = 0; t < RH_ACTIVE_N; t++) {
		uint32_t b = tcs + t * RH_TCS_STRIDE;
		uint32_t st = pt_rd(rh_drv, b + L->status), ctl = pt_rd(rh_drv, b + L->control);
		uint32_t en = pt_rd(rh_drv, b + L->enable), wt = pt_rd(rh_drv, b + L->wait);
		printf("PANDORA TOUCH: ACTIVE TCS%u idle=%#x control=%#x enable=%#x wait=%#x\n",
		    t, st, ctl, en, wt);
		if (chosen < 0 && st != 0 && st != 0xffffffffu && ctl == 0 && en == 0 && wt == 0 &&
		    !(pt_rd(rh_drv, tcs + 4) & (1u << t))) {
			chosen = (int)t;
		}
	}
	if (chosen < 0) {
		printf("PANDORA TOUCH: no free ACTIVE TCS; not voting\n");
		return EBUSY;
	}
	unsigned t = (unsigned)chosen;
	uint32_t b = tcs + t * RH_TCS_STRIDE, bit = 1u << t;
	uint32_t irq_en = pt_rd(rh_drv, tcs + 0);
	if (!rh_sync(tcs + 0, irq_en & ~bit)) {
		return EIO;
	}
	static const uint32_t offs[3] = {0, 8, 4}, vals[3] = {1800, 7, 1};
	for (unsigned c = 0; c < 3; c++) {
		uint32_t cmd = b + c * L->cmd_stride;
		if (!rh_sync(cmd + L->msgid, RH_WRITE_ACK) || !rh_sync(cmd + L->addr, address + offs[c]) ||
		    !rh_sync(cmd + L->data, vals[c])) {
			return EIO;
		}
	}
	if (!rh_sync(b + L->wait, 7) || !rh_sync(b + L->enable, 7) ||
	    !rh_sync(b + L->control, 0) || !rh_sync(b + L->control, 1u << 16)) {
		return EIO;
	}
	pt_wr(rh_drv, b + L->control, (1u << 16) | (1u << 24));
	bool done = false;
	uint32_t cs[3] = {0, 0, 0};
	for (unsigned us = 0; us < 100000 && !done; us++) {
		bool all = true;
		for (unsigned c = 0; c < 3; c++) {
			cs[c] = pt_rd(rh_drv, b + L->cmd_status + c * L->cmd_stride);
			all &= cs[c] != 0xffffffffu && (cs[c] & 0x10100u) == 0x10100u;
		}
		done = all && pt_rd(rh_drv, b + L->status) != 0;
		if (!done) {
			delay(1);
		}
	}
	printf("PANDORA TOUCH: L4I_E0 1800 mV vote TCS%u done=%d irq=%#x cmd-status=%#x/%#x/%#x\n",
	    t, done, pt_rd(rh_drv, tcs + 4), cs[0], cs[1], cs[2]);
	if (!done) {
		return ETIMEDOUT;
	}
	(void)rh_sync(b + L->control, 1u << 16);
	(void)rh_sync(b + L->control, 0);
	(void)rh_sync(b + L->enable, 0);
	(void)rh_sync(b + L->wait, 0);
	pt_wr(rh_drv, tcs + 8, bit);
	(void)rh_sync(tcs + 0, irq_en);
	return 0;
}

/* ---- TouchComm ----------------------------------------------------------------- */

static uint8_t
tc_crc6(const uint8_t *p, unsigned bits)
{
	static const uint16_t table[16] = {
		0, 268, 536, 788, 1072, 1340, 1576, 1828,
		2144, 2412, 2680, 2932, 3152, 3420, 3656, 3908
	};
	uint16_t r = 0x3f << 2;
	for (; bits > 8; bits -= 8) {
		r ^= *p++;
		r = (uint16_t)((r << 4) ^ table[(r >> 4) & 0xf]);
		r = (uint16_t)((r << 4) ^ table[(r >> 4) & 0xf]);
	}
	if (bits > 0) {
		uint16_t x = *p;
		while (bits--) {
			if (x & 0x80) {
				r ^= 0x80;
			}
			x <<= 1;
			r <<= 1;
			if (r & 0x100) {
				r ^= 0x03 << 2;
			}
		}
	}
	return (uint8_t)((r >> 2) & 0x3f);
}

/* CRC-16/CCITT, MSB first: Synaptics' table version computes the same. */
static uint16_t
tc_crc16(const uint8_t *p, unsigned n, uint16_t r)
{
	while (n--) {
		r ^= (uint16_t)(*p++ << 8);
		for (int i = 0; i < 8; i++) {
			r = (r & 0x8000) ? (uint16_t)((r << 1) ^ 0x1021) : (uint16_t)(r << 1);
		}
	}
	return r;
}

static void
tc_hex(const char *what, const uint8_t *p, unsigned n)
{
	printf("PANDORA TOUCH: %s (%u):", what, n);
	for (unsigned i = 0; i < n && i < 64; i++) {
		printf(" %02x", p[i]);
	}
	printf("%s\n", n > 64 ? " ..." : "");
}

/* v1: one message (header, then a continued read for the payload). */
static int
tc1_read(uint8_t *code)
{
	uint8_t h[4];
	int err = pt_spi(NULL, h, 4);
	if (err) {
		return err;
	}
	if (h[0] != TC_MARKER) {
		tc_hex("v1 bad header", h, 4);
		return EIO;
	}
	unsigned len = h[2] | (h[3] << 8);
	*code = h[1];
	pt.msg_len = 0;
	if (len == 0) {
		return 0;
	}
	if (len > PT_MSG_MAX || len + 6 > PT_XFER_MAX) {
		printf("PANDORA TOUCH: v1 code %#x length %u too long\n", h[1], len);
		return EIO;
	}
	unsigned n = 2 + len + 1 + (pt.crc ? 2 : 0);
	if ((err = pt_spi(NULL, pt_buf, n))) {
		return err;
	}
	if (pt_buf[0] != TC_MARKER || pt_buf[1] != TC_CONTINUED) {
		tc_hex("v1 bad continued read", pt_buf, n);
		return EIO;
	}
	memcpy(pt.msg, pt_buf + 2, len);
	pt.msg_len = len;
	return 0;
}

static int
tc1_write(uint8_t cmd, const uint8_t *pl, unsigned n)
{
	uint8_t b[64];
	if (n + 5 > sizeof(b)) {
		return EINVAL;
	}
	b[0] = cmd;
	b[1] = (uint8_t)n;
	b[2] = (uint8_t)(n >> 8);
	if (n) {
		memcpy(b + 3, pl, n);
	}
	unsigned size = n + 3;
	if (pt.crc) {
		uint16_t c = tc_crc16(b, size, 0xffff);
		b[size++] = (uint8_t)c;
		b[size++] = (uint8_t)(c >> 8);
	}
	return pt_spi(b, NULL, size);
}

static int
tc2_write(uint8_t cmd, const uint8_t *pl, unsigned n, bool resend)
{
	uint8_t b[64];
	if (n + 6 > sizeof(b)) {
		return EINVAL;
	}
	if (resend) {
		pt.seq--;
	}
	b[0] = cmd;
	b[1] = (uint8_t)n;
	b[2] = (uint8_t)(n >> 8);
	b[3] = (uint8_t)((pt.seq++ & 1u) << 6);
	b[3] |= tc_crc6(b, 26);
	unsigned size = 4;
	if (n) {
		memcpy(b + 4, pl, n);
		size += n;
		uint16_t c = tc_crc16(b, size, 0xffff);
		b[size++] = (uint8_t)(c >> 8);
		b[size++] = (uint8_t)c;
	}
	return pt_spi(b, NULL, size);
}

/* v2: the response to the command just written: header, then ACK + read per chunk. */
static int
tc2_response(uint8_t *code)
{
	uint8_t h[4];
	int err = pt_spi(NULL, h, 4);
	if (err) {
		return err;
	}
	if (tc_crc6(h, 32) != 0) {
		tc_hex("v2 header CRC bad", h, 4);
		return EIO;
	}
	unsigned len = h[1] | (h[2] << 8), off = 0, retries = 0;
	*code = h[0];
	pt.msg_len = 0;
	if (len > PT_MSG_MAX) {
		printf("PANDORA TOUCH: v2 code %#x length %u too long\n", h[0], len);
		return EIO;
	}
	bool retry = false;
	while (off < len) {
		unsigned room = (pt.max_rd ? pt.max_rd : 256) - 6, chunk = len - off;
		chunk = chunk > room ? room : chunk;
		if ((err = tc2_write(retry ? CMD_V2_RETRY : CMD_V2_ACK, NULL, 0, false)) ||
		    (err = pt_spi(NULL, pt_buf, chunk + 6))) {
			return err;
		}
		if (tc_crc6(pt_buf, 32) != 0 || tc_crc16(pt_buf, chunk + 6, 0xffff) != 0) {
			if (++retries > 5) {
				tc_hex("v2 continued read CRC bad", pt_buf, chunk + 6);
				return EIO;
			}
			retry = true;
			continue;
		}
		retry = false;
		retries = 0;
		memcpy(pt.msg + off, pt_buf + 4, chunk);
		off += chunk;
	}
	pt.msg_len = len;
	return 0;
}

static int
tc2_exchange(uint8_t cmd, const uint8_t *pl, unsigned n, uint8_t *code)
{
	int err;
	for (unsigned tries = 0; tries < 5; tries++) {
		if ((err = tc2_write(cmd, pl, n, tries > 0))) {
			return err;
		}
		delay(200);
		err = tc2_response(code);
		if (err == 0 && *code != 0x08 && *code != 0xfe) {   /* retry requested, corrupted */
			return 0;
		}
		delay(1000);
	}
	return err ? err : EIO;
}

/* Next message from the IC: v1 reads one, v2 asks for one (GET_REPORT). */
static int
tc_read(uint8_t *code)
{
	return pt.version == 1 ? tc1_read(code) : tc2_exchange(CMD_V2_GET_REPORT, NULL, 0, code);
}

static void tc_touch_report(const uint8_t *r, unsigned n);

static void
tc_report(uint8_t code)
{
	pt.reports++;
	if (code == TC_TOUCH) {
		pt.touch_reports++;
		tc_touch_report(pt.msg, pt.msg_len);
		return;
	}
	if (!(pt.codes_seen[code >> 5] & (1u << (code & 31)))) {
		pt.codes_seen[code >> 5] |= 1u << (code & 31);
		printf("PANDORA TOUCH: first report %#x\n", code);
		tc_hex("report", pt.msg, pt.msg_len);
	}
}

/* Write a command and wait (ATTN or 10 ms polls, 3 s) for its response; reports that
 * arrive meanwhile are handled. The response payload is left in pt.msg. */
static int
tc_command(uint8_t cmd, const uint8_t *pl, unsigned n, uint8_t *resp)
{
	uint8_t code = 0;
	int err;
	if (pt.version == 1) {
		err = tc1_write(cmd, pl, n);
	} else {
		err = tc2_exchange(cmd, pl, n, &code);
		if (!err && code != TC_ACK && code < TC_IDENTIFY) {
			*resp = code;
			return code == TC_OK ? 0 : EIO;
		}
		if (!err && code == TC_IDENTIFY && cmd == CMD_IDENTIFY) {
			*resp = code;
			return 0;
		}
		if (!err && code >= TC_IDENTIFY) {
			tc_report(code);
		}
	}
	if (err) {
		return err;
	}
	for (unsigned t = 0; t < 300; t++) {
		pt_sleep(10);
		if ((err = tc_read(&code))) {
			continue;
		}
		if (code == 0 || code == TC_NO_REPORT || code == TC_ACK) {
			continue;
		}
		if (code >= TC_IDENTIFY) {
			if (code == TC_IDENTIFY && cmd == CMD_IDENTIFY) {
				*resp = code;
				return 0;
			}
			tc_report(code);
			continue;
		}
		*resp = code;
		return code == TC_OK ? 0 : EIO;
	}
	printf("PANDORA TOUCH: command %#x: no response\n", cmd);
	return ETIMEDOUT;
}

static unsigned
le16(const uint8_t *p)
{
	return p[0] | (p[1] << 8);
}

static int
tc_identify_parse(void)
{
	if (pt.msg_len < 24) {
		tc_hex("identify too short", pt.msg, pt.msg_len);
		return EIO;
	}
	char part[17];
	memcpy(part, pt.msg + 2, 16);
	part[16] = 0;
	for (unsigned i = 0; i < 16; i++) {
		if (part[i] && (part[i] < 0x20 || part[i] > 0x7e)) {
			part[i] = '?';
		}
	}
	pt.max_wr = le16(pt.msg + 22);
	pt.max_rd = pt.msg_len >= 26 ? le16(pt.msg + 24) : 0;
	if (pt.max_rd && pt.max_rd < 16) {
		pt.max_rd = 0;
	}
	if (pt.max_rd > PT_XFER_MAX) {
		pt.max_rd = PT_XFER_MAX;
	}
	printf("PANDORA TOUCH: identify: TouchComm v%u version %u mode %#x part \"%s\" build %u "
	    "max write %u read %u\n", pt.version, pt.msg[0], pt.msg[1], part,
	    pt.msg[18] | (pt.msg[19] << 8) | (pt.msg[20] << 16) | ((uint32_t)pt.msg[21] << 24),
	    pt.max_wr, pt.max_rd);
	if (pt.msg[1] != 0x01) {
		printf("PANDORA TOUCH: not in application firmware (mode %#x); stopping\n", pt.msg[1]);
		return ENODEV;
	}
	return 0;
}

/* After reset: find the protocol from the first header and get the identify info. */
static int
tc_detect(void)
{
	uint8_t h[4], code = 0;
	int err = pt_spi(NULL, h, 4);
	if (err) {
		return err;
	}
	tc_hex("first header after reset", h, 4);
	if (h[0] == TC_MARKER) {
		pt.version = 1;
		pt.crc = true;              /* found out from the identify packet's tail */
		if (h[1] == TC_IDENTIFY) {
			unsigned len = le16(h + 2);
			unsigned n = 2 + len + 3;
			if (len < 24 || len > 64) {
				tc_hex("identify header", h, 4);
				return EIO;
			}
			if ((err = pt_spi(NULL, pt_buf, n))) {
				return err;
			}
			tc_hex("identify continued read", pt_buf, n);
			if (pt_buf[0] != TC_MARKER || pt_buf[1] != TC_CONTINUED) {
				return EIO;
			}
			memcpy(pt.msg, pt_buf + 2, len);
			pt.msg_len = len;
			pt.crc = !(pt_buf[2 + len + 1] == TC_PAD && pt_buf[2 + len + 2] == TC_PAD);
		} else {
			pt.crc = false;
			if ((err = tc_command(CMD_IDENTIFY, NULL, 0, &code))) {
				return err;
			}
		}
		printf("PANDORA TOUCH: TouchComm v1, CRC %s\n", pt.crc ? "on" : "off");
	} else if (tc_crc6(h, 32) == 0 && !(h[0] == 0 && h[1] == 0 && h[2] == 0)) {
		pt.version = 2;
		if ((err = tc_command(CMD_IDENTIFY, NULL, 0, &code))) {
			return err;
		}
		printf("PANDORA TOUCH: TouchComm v2 (identify answered %#x)\n", code);
	} else {
		printf("PANDORA TOUCH: no TouchComm header (IC unpowered or SPI wrong)\n");
		return ENODEV;
	}
	return tc_identify_parse();
}

static int
tc_setup(void)
{
	uint8_t code = 0;
	int err;
	if ((err = tc_command(CMD_APP_INFO, NULL, 0, &code))) {
		printf("PANDORA TOUCH: application info failed %d (code %#x)\n", err, code);
		return err;
	}
	if (pt.msg_len < 38) {
		tc_hex("application info too short", pt.msg, pt.msg_len);
		return EIO;
	}
	pt.max_x = le16(pt.msg + 32);
	pt.max_y = le16(pt.msg + 34);
	pt.max_objects = le16(pt.msg + 36);
	printf("PANDORA TOUCH: app info: version %u status %u max %u x %u objects %u image %u x %u\n",
	    le16(pt.msg), le16(pt.msg + 2), pt.max_x, pt.max_y, pt.max_objects,
	    pt.msg_len >= 44 ? le16(pt.msg + 40) : 0, pt.msg_len >= 44 ? le16(pt.msg + 42) : 0);
	if (pt.max_objects == 0 || pt.max_objects > PT_MAX) {
		pt.max_objects = PT_MAX;
	}
	if (pt.max_x == 0 || pt.max_y == 0) {
		return EIO;
	}
	if ((err = tc_command(CMD_GET_TOUCH_CONFIG, NULL, 0, &code))) {
		printf("PANDORA TOUCH: touch report config failed %d (code %#x)\n", err, code);
		return err;
	}
	pt.config_len = pt.msg_len < sizeof(pt.config) ? pt.msg_len : sizeof(pt.config);
	memcpy(pt.config, pt.msg, pt.config_len);
	tc_hex("touch report config", pt.config, pt.config_len);
	return pt.config_len ? 0 : EIO;
}

/* syna_tcm_get_touch_data: little-endian bit field at a bit offset. */
static unsigned
tc_bits(const uint8_t *r, unsigned n, unsigned off, unsigned bits)
{
	if (bits == 0 || bits > 32 || off + bits > n * 8) {
		return 0;
	}
	unsigned out = 0, done = 0;
	while (done < bits) {
		unsigned bo = off % 8, take = 8 - bo;
		take = take < bits - done ? take : bits - done;
		out |= ((unsigned)(r[off / 8] >> bo) & (0xffu >> (8 - take))) << done;
		off += take;
		done += take;
	}
	return out;
}

/* Touch report, decoded with the IC's own report config (syna_tcm_parse_touch_report). */
static void
tc_touch_report(const uint8_t *r, unsigned n)
{
	unsigned status[PT_MAX] = {0}, xs[PT_MAX] = {0}, ys[PT_MAX] = {0};
	unsigned idx = 0, off = 0, obj = 0, next = 0, objects = 0, active_objects = 0;
	bool active_only = false, have_count = false;
	static unsigned end_of_foreach;
	while (idx < pt.config_len) {
		uint8_t c = pt.config[idx++];
		unsigned bits, v;
		switch (c) {
		case 0x00:
			goto done;
		case 0x01:          /* for each active object */
			obj = 0;
			next = idx;
			active_only = true;
			break;
		case 0x02:          /* for each object */
			obj = 0;
			next = idx;
			active_only = false;
			break;
		case 0x03:          /* end for each */
			end_of_foreach = idx;
			if (active_only) {
				if (have_count) {
					objects++;
					obj++;
					if (objects < active_objects) {
						idx = next;
					}
				} else if (off < n * 8) {
					obj++;
					idx = next;
				}
			} else {
				obj++;
				if (obj < pt.max_objects) {
					idx = next;
				}
			}
			break;
		case 0x04:          /* pad to next byte */
			off = (off + 7) / 8 * 8;
			break;
		default:
			if (idx >= pt.config_len) {
				goto done;
			}
			bits = pt.config[idx++];
			v = tc_bits(r, n, off, bits);
			off += bits;
			if (c == 0x06) {
				obj = v;
			} else if (obj < PT_MAX && c == 0x07) {
				status[obj] = v;
			} else if (obj < PT_MAX && c == 0x08) {
				xs[obj] = v;
			} else if (obj < PT_MAX && c == 0x09) {
				ys[obj] = v;
			} else if (c == 0x18) {
				active_objects = v;
				have_count = true;
				if (v == 0) {
					if (end_of_foreach == 0) {
						goto done;
					}
					idx = end_of_foreach;
				}
			}
			break;
		}
		if (obj > 64) {
			break;
		}
	}
done:
	if (sweet_fb_display_off) {
		return;
	}
	uint32_t mask = 0;
	for (unsigned id = 0; id < PT_MAX; id++) {
		/* finger, glove, stylus, eraser, small object */
		if (status[id] < 1 || status[id] > 5) {
			continue;
		}
		mask |= 1u << id;
		/* IC units -> the presented 1125x2436 display (SweetHID.cpp on PANDORA). */
		uint64_t px = (uint64_t)xs[id] * 1220 / (pt.max_x + 1);
		uint64_t py = (uint64_t)ys[id] * 2656 / (pt.max_y + 1);
		int x, y;
		if (pt.fill) {
			x = (int)((uint64_t)xs[id] * 1125 / (pt.max_x + 1));
			y = (int)((uint64_t)ys[id] * 2436 / (pt.max_y + 1));
		} else {
			x = (int)px - (1220 - 1125) / 2;
			y = (int)py - (2656 - 2436) / 2;
		}
		pt.x[id] = x;
		pt.y[id] = y;
		sweet_hid_touch((int)id, 1, x, y);
	}
	for (unsigned id = 0; id < PT_MAX; id++) {
		if ((pt.active & (1u << id)) && !(mask & (1u << id))) {
			sweet_hid_touch((int)id, 0, pt.x[id], pt.y[id]);
		}
	}
	if (mask != pt.active && pt.touch_reports <= 20) {
		printf("PANDORA TOUCH: report %u: fingers %#x (first %u,%u raw)\n", pt.touch_reports,
		    mask, xs[0], ys[0]);
	}
	pt.active = mask;
}

/* ---- thread -------------------------------------------------------------------- */

static void
pt_thread(void *arg, wait_result_t result)
{
	unsigned mask = 0, delay_s = 5;
	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("pandora_touch", &mask, sizeof(mask));
	(void)PE_parse_boot_argn("pandora_touchdelay", &delay_s, sizeof(delay_s));
	(void)PE_parse_boot_argn("pandora_steps", &pt.steps_ms, sizeof(pt.steps_ms));
	int fill = 0;
	(void)PE_parse_boot_argn("sweetfb_fill", &fill, sizeof(fill));
	pt.fill = fill != 0;
	printf("PANDORA TOUCH: pandora_touch=%#x, starting in %u s\n", mask, delay_s);
	pt_sleep(delay_s * 1000u);
	pt.gcc = ml_io_map(PT_GCC_PA, 0x1f0000);
	pt.tlmm = ml_io_map(PT_TLMM_PA, 0xa0000);
	pt.se = ml_io_map(PT_SE_PA, 0x4000);
	if (!pt.gcc || !pt.tlmm || !pt.se) {
		goto out;
	}
	pt_step("GCC wrapper 4 clocks", PT_GCC_PA + GCC_VOTE);
	pt_clock_dump("before");
	pt_step("TLMM GPIO 117 (not shared)", PT_TLMM_PA + PT_GPIO(PT_AVDD));
	(void)pt_rd(pt.tlmm, PT_GPIO(PT_AVDD));
	pt_step("TLMM GPIO 48 (trusted-touch VM list)", PT_TLMM_PA + PT_GPIO(48));
	pt_pins_dump("before");
	int err;
	if ((err = pt_clocks())) {
		printf("PANDORA TOUCH: clocks failed %d\n", err);
		goto out;
	}
	pt_clock_dump("after");
	pt_step("SE0 registers", PT_SE_PA);
	pt_se_dump("before");
	if (mask & 2u) {
		err = pt_rpmh_vote();
		printf("PANDORA TOUCH: L4I_E0 vote -> %d\n", err);
	}
	if (!(mask & 4u)) {
		goto out;
	}
	if ((err = pt_se_init())) {
		goto out;
	}
	pt_se_dump("after");
	/* Pins, then power and reset (syna_spi_power_on / hw_reset: 50 ms power delay,
	 * reset low 10 ms, 200 ms before the first read). */
	pt_gpio(PT_RESET, 0, 0, 2, 0);
	pt_gpio(PT_IRQ, 0, 0, 2, -1);
	for (unsigned p = 48; p <= 51; p++) {
		pt_gpio(p, 1, 1, 6, -1);
	}
	pt_gpio(PT_AVDD, 0, 0, 2, 1);
	pt_sleep(50);
	pt_gpio(PT_RESET, 0, 0, 2, 0);
	pt_sleep(10);
	pt_gpio(PT_RESET, 0, 0, 2, 1);
	pt_sleep(200);
	pt_pins_dump("after reset");
	if ((err = tc_detect()) || (err = tc_setup())) {
		printf("PANDORA TOUCH: TouchComm set-up failed %d\n", err);
		goto out;
	}
	printf("PANDORA TOUCH: running, polling GPIO %u\n", PT_IRQ);
	uint64_t active_abs, idle_abs, next = mach_absolute_time(), last_irq = 0, recent_abs;
	nanoseconds_to_absolutetime(4 * NSEC_PER_MSEC, &active_abs);
	nanoseconds_to_absolutetime(8 * NSEC_PER_MSEC, &idle_abs);
	nanoseconds_to_absolutetime(150 * NSEC_PER_MSEC, &recent_abs);
	unsigned errors = 0, polls = 0;
	for (;;) {
		uint64_t now = mach_absolute_time();
		if (++polls % 15000u == 0) {
			printf("PANDORA TOUCH: %u reports (%u touch), %u errors\n", pt.reports,
			    pt.touch_reports, errors);
		}
		for (unsigned k = 0; k < 4 && !(pt_rd(pt.tlmm, PT_GPIO(PT_IRQ) + 4) & 1u); k++) {
			uint8_t code = 0;
			last_irq = now;
			if ((err = tc_read(&code))) {
				if ((++errors % 100u) == 1) {
					printf("PANDORA TOUCH: read error %d (%u)\n", err, errors);
				}
				if (pt.active) {
					tc_touch_report(NULL, 0);
				}
				break;
			}
			if (code >= TC_IDENTIFY) {
				tc_report(code);
			} else if (code == 0 || code == TC_NO_REPORT) {
				break;
			}
		}
		if (pt.dead) {
			break;
		}
		uint64_t period = (pt.active || now - last_irq < recent_abs) ? active_abs : idle_abs;
		next += period;
		now = mach_absolute_time();
		if (next <= now) {
			next = now + period;
		}
		pt_sleep_until(next);
	}
out:
	printf("PANDORA TOUCH: stopped\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
pandora_touch_init(void)
{
	thread_t thread;
	unsigned mask = 0;
	if (pt.started) {
		return;
	}
	pt.started = true;
	(void)PE_parse_boot_argn("pandora_touch", &mask, sizeof(mask));
	if (!mask) {
		printf("PANDORA TOUCH: off (pandora_touch=0)\n");
		return;
	}
	if (kernel_thread_start_priority(pt_thread, NULL, PT_PRIORITY, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */

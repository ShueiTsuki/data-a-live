/*
 * Qualcomm GPI DMA engine for one GPII, polled (pandora_gpi.h). Sequences as Linux 6.6 gpi.c:
 * gpi_ch_init (event ring allocated and programmed, then both channels allocated, programmed and
 * started), gpi_issue_pending (TREs copied to the ring, doorbell = next free element),
 * gpi_process_events (the event ring's hardware pointer in its context word 4, events consumed,
 * doorbell = last element handed back), and the I2C split of i2c-qcom-geni.c geni_i2c_gpi_xfer.
 * Command completion is read from the channel state in context word 0 instead of the interrupt.
 */
#if defined(PANDORA)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <arm/caches_internal.h>
#include "pandora_gpi.h"

extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);

/* Registers (GPII n, channel k), relative to block - ee_offset. */
#define CH_CNTXT(n, k)     (0x20000u + 0x4000u * (n) + 0x80u * (k))
#define CH_QOS(n, k)       (0x2005cu + 0x4000u * (n) + 0x80u * (k))
#define CH_SCRATCH(n, k, i) (0x20060u + 0x4000u * (n) + 0x80u * (k) + 4u * (i))
#define EV_CNTXT(n)        (0x21000u + 0x4000u * (n))
#define CH_DB(n, k)        (0x22000u + 0x4000u * (n) + 0x8u * (k))
#define EV_DB(n)           (0x22100u + 0x4000u * (n))
#define CH_CMD(n)          (0x23008u + 0x4000u * (n))
#define EV_CMD(n)          (0x23010u + 0x4000u * (n))
#define TYPE_IRQ(n)        (0x23080u + 0x4000u * (n))
#define TYPE_IRQ_MSK(n)    (0x23088u + 0x4000u * (n))
#define SRC_CH_IRQ(n)      (0x23090u + 0x4000u * (n))
#define SRC_EV_IRQ(n)      (0x23094u + 0x4000u * (n))
#define SRC_CH_IRQ_MSK(n)  (0x23098u + 0x4000u * (n))
#define SRC_EV_IRQ_MSK(n)  (0x2309cu + 0x4000u * (n))
#define SRC_CH_IRQ_CLR(n)  (0x230a0u + 0x4000u * (n))
#define SRC_EV_IRQ_CLR(n)  (0x230a4u + 0x4000u * (n))
#define IEOB_IRQ_MSK(n)    (0x230b8u + 0x4000u * (n))
#define IEOB_IRQ_CLR(n)    (0x230c0u + 0x4000u * (n))
#define GLOB_IRQ_STTS(n)   (0x23100u + 0x4000u * (n))
#define GLOB_IRQ_EN(n)     (0x23108u + 0x4000u * (n))
#define GLOB_IRQ_CLR(n)    (0x23110u + 0x4000u * (n))
#define GPII_IRQ_STTS(n)   (0x23118u + 0x4000u * (n))
#define GPII_IRQ_EN(n)     (0x23120u + 0x4000u * (n))
#define GPII_IRQ_CLR(n)    (0x23128u + 0x4000u * (n))
#define INTSET(n)          (0x23180u + 0x4000u * (n))
#define MSI_LSB(n)         (0x23188u + 0x4000u * (n))
#define MSI_MSB(n)         (0x2318cu + 0x4000u * (n))
#define ERROR_LOG(n)       (0x23200u + 0x4000u * (n))
#define CNTXT_SCRATCH(n, i) (0x23400u + 0x4000u * (n) + 4u * (i))

/* Context words. */
#define CX_CONFIG     0x00u
#define CX_LENGTH     0x04u
#define CX_BASE_LSB   0x08u
#define CX_BASE_MSB   0x0cu
#define CX_RP_LSB     0x10u
#define CX_INT_MOD    0x20u
#define CX_MSI_LSB    0x28u
#define CX_MSI_MSB    0x2cu
#define CX_RPU_LSB    0x30u
#define CX_RPU_MSB    0x34u

/* Commands: opcode << 24 | channel. */
#define OP_ALLOCATE   0u
#define OP_START      1u
#define OP_STOP       2u
#define OP_RESET      9u
#define OP_DEALLOC    10u
#define ST_NOT_ALLOC  0u
#define ST_ALLOCATED  1u
#define ST_STARTED    2u
#define ST_STOPPED    3u

/* Memory: TX ring, RX ring (64 TREs each), event ring (128 events, ev-factor 1), bounce buffers.
 * Each ring sits at a multiple of its own (power of two) length, as gpi_alloc_ring aligns them. */
#define RING_LEN      (64u * PGPI_EL_SIZE)
#define EV_LEN        (128u * PGPI_EL_SIZE)
#define TX_RING       0x0000u
#define RX_RING       0x0400u
#define EV_RING       0x0800u
#define TX_BUF        0x1000u
#define RX_BUF        0x1800u
#define BUF_MAX       0x800u
#define MEM_LEN       0x2000u

#define PROTO_GPI     2u      /* channel context protocol: GPI */
#define CMD_TIMEOUT_MS 250u
#define XFER_TIMEOUT_MS 100u

static uint32_t
rd(const PandoraGpi *g, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(g->regs + off - g->ee_offset);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
wr(const PandoraGpi *g, uint32_t off, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(g->regs + off - g->ee_offset) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

/* A command, then its channel (or the event ring) in STATE; the control interrupt status cleared. */
static int
command(PandoraGpi *g, bool ev, uint32_t op, uint32_t chid, uint32_t state)
{
	const uint32_t n = g->gpii, cx = ev ? EV_CNTXT(n) : CH_CNTXT(n, chid);
	wr(g, ev ? EV_CMD(n) : CH_CMD(n), op << 24 | (ev ? 0u : chid));
	uint32_t now = 0;
	for (unsigned t = 0; t < CMD_TIMEOUT_MS * 10u; t++) {
		now = (rd(g, cx + CX_CONFIG) >> 20) & 0xfu;
		if (now == state) {
			break;
		}
		delay(100);
	}
	wr(g, ev ? SRC_EV_IRQ_CLR(n) : SRC_CH_IRQ_CLR(n), ev ? 1u : 1u << chid);
	if (now != state) {
		printf("PANDORA GPI: gpii %u %s %u opcode %u: state %u, wanted %u (type irq %#x glob %#x gpii %#x log %#x)\n", n,
		    ev ? "event ring" : "channel", chid, op, now, state, rd(g, TYPE_IRQ(n)), rd(g, GLOB_IRQ_STTS(n)),
		    rd(g, GPII_IRQ_STTS(n)), rd(g, ERROR_LOG(n)));
		return ETIMEDOUT;
	}
	return 0;
}

static void
put_tre(PandoraGpi *g, bool rx, const PgpiTre *t)
{
	uint32_t *wp = rx ? &g->rx_wp : &g->tx_wp;
	memcpy((void *)(g->mem + (rx ? RX_RING : TX_RING) + *wp), t, sizeof(*t));
	*wp = (*wp + PGPI_EL_SIZE) % RING_LEN;
}

static void
doorbell(PandoraGpi *g, bool rx)
{
	const uint32_t base = rx ? RX_RING : TX_RING, wp = rx ? g->rx_wp : g->tx_wp;
	clean_dcache64((addr64_t)(g->mem_pa + base), RING_LEN, TRUE);
	wr(g, CH_DB(g->gpii, rx ? 1u : 0u), (uint32_t)(g->mem_pa + base + wp));
}

/* Events until one completes channel CHID's transfer (IEOT); 0, EIO on an error code, ETIMEDOUT. */
static int
wait_done(PandoraGpi *g, uint32_t chid, uint32_t *len)
{
	const uint32_t n = g->gpii;
	for (unsigned t = 0; t < XFER_TIMEOUT_MS * 10u; t++) {
		const uint32_t hw = (uint32_t)(rd(g, EV_CNTXT(n) + CX_RP_LSB) - (uint32_t)(g->mem_pa + EV_RING));
		if (hw == g->ev_rp) {
			delay(100);
			continue;
		}
		flush_dcache64((addr64_t)(g->mem_pa + EV_RING), EV_LEN, TRUE);
		int result = -1;
		while (g->ev_rp != hw) {
			const PgpiEvent e = pgpi_event((const uint32_t *)(g->mem + EV_RING + g->ev_rp));
			if (!g->quiet || e.type != PGPI_EV_XFER_DONE || e.code == PGPI_CODE_ERROR) {
				printf("PANDORA GPI: event type %#x channel %u code %u status %#x length %u tre %#llx\n", e.type,
				    e.chid, e.code, e.status, e.length, e.tre);
			}
			if (e.type == PGPI_EV_XFER_DONE && e.chid == chid && result < 0) {
				result = e.code == PGPI_CODE_ERROR ? EIO : 0;
				*len = e.length;
			}
			g->ev_rp = (g->ev_rp + PGPI_EL_SIZE) % EV_LEN;
			g->ev_wp = (g->ev_wp + PGPI_EL_SIZE) % EV_LEN;
		}
		wr(g, EV_DB(n), (uint32_t)(g->mem_pa + EV_RING + g->ev_wp));
		wr(g, IEOB_IRQ_CLR(n), 1);
		if (result >= 0) {
			return result;
		}
	}
	printf("PANDORA GPI: gpii %u channel %u: no completion in %u ms (type irq %#x glob %#x gpii %#x log %#x, "
	    "tx state %u rx state %u)\n", n, chid, XFER_TIMEOUT_MS, rd(g, TYPE_IRQ(n)), rd(g, GLOB_IRQ_STTS(n)),
	    rd(g, GPII_IRQ_STTS(n)), rd(g, ERROR_LOG(n)), (rd(g, CH_CNTXT(n, 0)) >> 20) & 0xfu,
	    (rd(g, CH_CNTXT(n, 1)) >> 20) & 0xfu);
	return ETIMEDOUT;
}

int
pandora_gpi_open(PandoraGpi *g, uint64_t block_pa, uint32_t block_len, uint32_t ee_offset, uint32_t gpii,
    uint32_t seid, uint32_t proto)
{
	const vm_offset_t regs = g->regs, mem = g->mem;
	const uint64_t mem_pa = g->mem_pa;
	const bool quiet = g->quiet;
	bzero(g, sizeof(*g));
	g->ee_offset = ee_offset;
	g->gpii = gpii;
	g->seid = seid;
	g->proto = proto;
	g->quiet = quiet;
	g->regs = regs ? regs : ml_io_map_unmappable((vm_offset_t)block_pa, block_len, VM_WIMG_IO);
	if (!g->regs) {
		return ENOMEM;
	}
	g->mem = mem;
	g->mem_pa = mem_pa;
	const uint32_t n = gpii;
	if (!quiet) {
		printf("PANDORA GPI: gpii %u before: tx %#x rx %#x event %#x, type irq %#x, masks %#x/%#x/%#x\n", n,
		    rd(g, CH_CNTXT(n, 0)), rd(g, CH_CNTXT(n, 1)), rd(g, EV_CNTXT(n)), rd(g, TYPE_IRQ(n)),
		    rd(g, TYPE_IRQ_MSK(n)), rd(g, SRC_CH_IRQ_MSK(n)), rd(g, IEOB_IRQ_MSK(n)));
	}
	if (((rd(g, CH_CNTXT(n, 0)) | rd(g, CH_CNTXT(n, 1)) | rd(g, EV_CNTXT(n))) >> 20) & 0xfu) {
		if (!quiet) {
			printf("PANDORA GPI: gpii %u is in use; left alone\n", n);
		}
		return EBUSY;
	}
	g->owned = true;
	if (!g->mem) {
		if (kmem_alloc_contig(kernel_map, &g->mem, MEM_LEN, PAGE_MASK, 0, 0, KMA_DATA | KMA_ZERO | KMA_PERMANENT,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			g->mem = 0;
			return ENOMEM;
		}
		g->mem_pa = ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)g->mem));
	}
	bzero((void *)g->mem, MEM_LEN);
	clean_dcache64((addr64_t)g->mem_pa, MEM_LEN, TRUE);

	/* interrupts as gpi_config_interrupts; nothing takes them, the status is polled */
	wr(g, TYPE_IRQ_MSK(n), 0x4fu);          /* general, IEOB, global, event and channel control */
	wr(g, IEOB_IRQ_MSK(n), 1);
	wr(g, SRC_CH_IRQ_MSK(n), 3);
	wr(g, SRC_EV_IRQ_MSK(n), 1);
	wr(g, GLOB_IRQ_EN(n), 0xf);
	wr(g, GPII_IRQ_EN(n), 0xf);
	wr(g, MSI_LSB(n), 0);
	wr(g, MSI_MSB(n), 0);
	wr(g, CNTXT_SCRATCH(n, 0), 0);
	wr(g, CNTXT_SCRATCH(n, 1), 0);
	wr(g, INTSET(n), 1);
	wr(g, ERROR_LOG(n), 0);

	/* event ring (gpi_alloc_ev_chan) */
	int e = command(g, true, OP_ALLOCATE, 0, ST_ALLOCATED);
	if (e) {
		return e;
	}
	const uint64_t ev = g->mem_pa + EV_RING;
	wr(g, EV_CNTXT(n) + CX_CONFIG, PGPI_EL_SIZE << 24 | 1u << 16 | 2u);   /* IRQ interrupt type, GPI event ring */
	wr(g, EV_CNTXT(n) + CX_LENGTH, EV_LEN);
	wr(g, EV_CNTXT(n) + CX_BASE_LSB, (uint32_t)ev);
	wr(g, EV_CNTXT(n) + CX_BASE_MSB, (uint32_t)(ev >> 32));
	wr(g, EV_DB(n) + 4, (uint32_t)(ev >> 32));
	wr(g, EV_CNTXT(n) + CX_INT_MOD, 0);
	wr(g, EV_CNTXT(n) + CX_MSI_LSB, 0);
	wr(g, EV_CNTXT(n) + CX_MSI_MSB, 0);
	wr(g, EV_CNTXT(n) + CX_RPU_LSB, 0);
	wr(g, EV_CNTXT(n) + CX_RPU_MSB, 0);
	g->ev_wp = EV_LEN - PGPI_EL_SIZE;   /* every element handed to the hardware */
	wr(g, EV_DB(n), (uint32_t)(ev + g->ev_wp));

	/* channels (gpi_alloc_chan): 0 = TX (direction out), 1 = RX */
	for (uint32_t k = 0; k < 2 && !e; k++) {
		const uint64_t ring = g->mem_pa + (k ? RX_RING : TX_RING);
		e = command(g, false, OP_ALLOCATE, k, ST_ALLOCATED);
		if (e) {
			break;
		}
		wr(g, CH_CNTXT(n, k) + CX_CONFIG, PGPI_EL_SIZE << 24 | 0u << 14 | (k ? 0u : 1u) << 3 | PROTO_GPI);
		wr(g, CH_CNTXT(n, k) + CX_LENGTH, RING_LEN);
		wr(g, CH_CNTXT(n, k) + CX_BASE_LSB, (uint32_t)ring);
		wr(g, CH_CNTXT(n, k) + CX_BASE_MSB, (uint32_t)(ring >> 32));
		wr(g, CH_DB(n, k) + 4, (uint32_t)(ring >> 32));
		wr(g, CH_SCRATCH(n, k, 0), (1u - k) << 16 | (proto & 0xfu) << 4 | (seid & 7u));
		wr(g, CH_SCRATCH(n, k, 1), 0);
		wr(g, CH_SCRATCH(n, k, 2), 0);
		wr(g, CH_SCRATCH(n, k, 3), 0);
		wr(g, CH_QOS(n, k), 1);
	}
	for (uint32_t k = 0; k < 2 && !e; k++) {
		e = command(g, false, OP_START, k, ST_STARTED);
	}
	if (!quiet || e) {
		printf("PANDORA GPI: gpii %u for SE %u: rings at %#llx, tx %#x rx %#x event %#x, err %d\n", n, seid,
		    g->mem_pa, rd(g, CH_CNTXT(n, 0)), rd(g, CH_CNTXT(n, 1)), rd(g, EV_CNTXT(n)), e);
	}
	g->open = e == 0;
	return e;
}

int
pandora_gpi_i2c(PandoraGpi *g, const PgpiI2cClock *clk, uint8_t addr, const uint8_t *w, unsigned nw, uint8_t *r,
    unsigned nr)
{
	if (!g->open || nw > BUF_MAX || nr > BUF_MAX || (!nw && !nr)) {
		return EINVAL;
	}
	uint32_t len = 0;
	int e = 0;
	bool config = true;
	if (nw) {
		/* the write: config, GO (write, no STOP when a read follows), the bytes; up to 8 go in an
		 * immediate TRE as Android sends them, longer writes as DMA from the TX buffer */
		PgpiTre t = pgpi_i2c_config0(clk);
		put_tre(g, false, &t);
		t = pgpi_i2c_go(PGPI_I2C_WRITE, addr, nr != 0, 0, 0);
		put_tre(g, false, &t);
		if (nw <= 8) {
			t = pgpi_imm(w, nw);
		} else {
			memcpy((void *)(g->mem + TX_BUF), w, nw);
			clean_dcache64((addr64_t)(g->mem_pa + TX_BUF), BUF_MAX, TRUE);
			t = pgpi_dma(g->mem_pa + TX_BUF, nw);
		}
		put_tre(g, false, &t);
		doorbell(g, false);
		e = wait_done(g, 0, &len);
		config = false;
	}
	if (!e && nr) {
		/* the read: (config,) GO (read, linked to RX) on TX, DMA into the RX buffer on RX; RX first */
		flush_dcache64((addr64_t)(g->mem_pa + RX_BUF), BUF_MAX, TRUE);
		PgpiTre t = pgpi_dma(g->mem_pa + RX_BUF, nr);
		put_tre(g, true, &t);
		if (config) {
			t = pgpi_i2c_config0(clk);
			put_tre(g, false, &t);
		}
		t = pgpi_i2c_go(PGPI_I2C_READ, addr, 0, nr, 1);
		put_tre(g, false, &t);
		doorbell(g, true);
		doorbell(g, false);
		e = wait_done(g, 1, &len);
		flush_dcache64((addr64_t)(g->mem_pa + RX_BUF), BUF_MAX, TRUE);
		memcpy(r, (const void *)(g->mem + RX_BUF), nr);
	}
	return e;
}

void
pandora_gpi_close(PandoraGpi *g)
{
	if (!g->regs || !g->owned) {
		return;
	}
	const uint32_t n = g->gpii;
	if (g->open) {
		for (uint32_t k = 0; k < 2; k++) {
			(void)command(g, false, OP_STOP, k, ST_STOPPED);
			(void)command(g, false, OP_RESET, k, ST_ALLOCATED);
		}
	}
	for (uint32_t k = 0; k < 2; k++) {
		wr(g, CH_CMD(n), OP_DEALLOC << 24 | k);   /* always succeeds; the state is not updated */
		delay(1000);
		wr(g, SRC_CH_IRQ_CLR(n), 1u << k);
	}
	(void)command(g, true, OP_RESET, 0, ST_ALLOCATED);
	wr(g, EV_CMD(n), OP_DEALLOC << 24);
	delay(1000);
	wr(g, SRC_EV_IRQ_CLR(n), 1);
	wr(g, TYPE_IRQ_MSK(n), 0);
	wr(g, IEOB_IRQ_MSK(n), 0);
	wr(g, SRC_CH_IRQ_MSK(n), 0);
	wr(g, SRC_EV_IRQ_MSK(n), 0);
	wr(g, GLOB_IRQ_EN(n), 0);
	wr(g, GPII_IRQ_EN(n), 0);
	wr(g, INTSET(n), 0);
	if (!g->quiet) {
		printf("PANDORA GPI: gpii %u closed: tx %#x rx %#x event %#x\n", n, rd(g, CH_CNTXT(n, 0)),
		    rd(g, CH_CNTXT(n, 1)), rd(g, EV_CNTXT(n)));
	}
	g->open = g->owned = false;
}
#endif /* PANDORA */

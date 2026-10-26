/*
 * Qualcomm GPI DMA (the QUP wrappers' GSI engine) for an I2C serial engine that runs in GSI-only mode.
 * PANDORA's fuel gauge bus (wrapper 1 SE5, 0xa94000) has FIFO_IF_DISABLE set: transfers go as
 * transfer ring elements (TREs) through one GPII's TX and RX channels, completions come back on its
 * event ring. Register map, ring contexts, commands and TRE formats as Linux 6.6
 * drivers/dma/qcom/gpi.c and drivers/i2c/busses/i2c-qcom-geni.c (GPI mode); polled, no interrupts.
 * The TRE and event encoders here are pure (host-tested, tools/hosttest); the engine is pandora_gpi.c.
 */
#ifndef PANDORA_GPI_H
#define PANDORA_GPI_H

#include <stdint.h>

/* Ring elements: TREs and events are four 32-bit words. */
#define PGPI_EL_SIZE       16u

/* dword 3 of a TRE */
#define PGPI_TRE_CHAIN     (1u << 0)
#define PGPI_TRE_IEOB      (1u << 8)
#define PGPI_TRE_IEOT      (1u << 9)
#define PGPI_TRE_BEI       (1u << 10)
#define PGPI_TRE_LINK      (1u << 11)
#define PGPI_TRE_DMA       0x10u
#define PGPI_TRE_GO        0x20u
#define PGPI_TRE_CONFIG0   0x22u

#define PGPI_I2C_WRITE     1u
#define PGPI_I2C_READ      2u

/* Events: type in dword 3 bits 23..16, channel in 31..24; code in dword 2 bits 31..24. */
#define PGPI_EV_XFER_DONE  0x22u
#define PGPI_EV_IMMEDIATE  0x30u
#define PGPI_EV_QUP_NOTIF  0x31u
#define PGPI_CODE_SUCCESS  1u
#define PGPI_CODE_EOT      2u
#define PGPI_CODE_EOB      4u
#define PGPI_CODE_ERROR    16u

typedef struct {
	uint32_t w[4];
} PgpiTre;

/* SCL timing in serial-engine clocks after the divider (i2c-qcom-geni's clock table). */
typedef struct {
	uint8_t clk_div, t_high, t_low, t_cycle;
} PgpiI2cClock;

static inline PgpiTre
pgpi_i2c_config0(const PgpiI2cClock *c)
{
	PgpiTre t;
	t.w[0] = (uint32_t)c->t_low | (uint32_t)c->t_high << 8 | (uint32_t)c->t_cycle << 16 | 3u << 24;   /* TX, RX pack */
	t.w[1] = 0;
	t.w[2] = c->clk_div & 0xfffu;
	t.w[3] = PGPI_TRE_CONFIG0 << 16 | PGPI_TRE_CHAIN;
	return t;
}

/* GO: OP to 7-bit ADDR; STRETCH keeps the bus (no STOP, a repeated start follows); RX_LEN bytes for
 * a read. LINK hands the transfer to the RX channel's DMA TRE, CHAIN to the next TRE on this ring. */
static inline PgpiTre
pgpi_i2c_go(uint32_t op, uint8_t addr, int stretch, uint32_t rx_len, int link)
{
	PgpiTre t;
	t.w[0] = (op & 0x1fu) | (uint32_t)(addr & 0x7fu) << 8 | (stretch ? 1u << 26 : 0u);
	t.w[1] = 0;
	t.w[2] = rx_len & 0xffffffu;
	t.w[3] = PGPI_TRE_GO << 16 | (link ? PGPI_TRE_LINK : PGPI_TRE_CHAIN);
	return t;
}

static inline PgpiTre
pgpi_dma(uint64_t pa, uint32_t len)
{
	PgpiTre t;
	t.w[0] = (uint32_t)pa;
	t.w[1] = (uint32_t)(pa >> 32);
	t.w[2] = len & 0xffffffu;
	t.w[3] = PGPI_TRE_DMA << 16 | PGPI_TRE_IEOT;
	return t;
}

typedef struct {
	uint64_t tre;          /* the TRE the event is about (bus address) */
	uint32_t length, code, status, type, chid;
} PgpiEvent;

static inline PgpiEvent
pgpi_event(const uint32_t w[4])
{
	PgpiEvent e;
	e.tre = (uint64_t)w[0] | (uint64_t)w[1] << 32;
	e.length = w[2] & 0xffffffu;
	e.code = w[2] >> 24;
	e.status = w[3] & 0xffffu;
	e.type = (w[3] >> 16) & 0xffu;
	e.chid = w[3] >> 24;
	return e;
}

#ifdef KERNEL
#include <stdbool.h>
#include <mach/vm_types.h>

/* One GPII with a TX and an RX channel for one serial engine. */
typedef struct {
	vm_offset_t regs;      /* the mapped GPI block */
	uint32_t ee_offset;    /* qcom,gpi-ee-offset: registers are at block - ee_offset + offset */
	uint32_t gpii, seid, proto;
	vm_offset_t mem;       /* rings and bounce buffers, physically contiguous */
	uint64_t mem_pa;
	uint32_t tx_wp, rx_wp; /* next free TRE, bytes into each ring */
	uint32_t ev_rp;        /* next event to look at, bytes into the event ring */
	uint32_t ev_wp;        /* last element handed to the hardware */
	bool open;
} PandoraGpi;

/* Allocate and start the GPII's event ring and both channels; logs every step. 0 or an errno. */
int pandora_gpi_open(PandoraGpi *g, uint64_t block_pa, uint32_t block_len, uint32_t ee_offset, uint32_t gpii,
    uint32_t seid, uint32_t proto);
/* Write NW bytes (if any) to ADDR, then with a repeated start read NR bytes (if any). 0 or an errno. */
int pandora_gpi_i2c(PandoraGpi *g, const PgpiI2cClock *clk, uint8_t addr, const uint8_t *w, unsigned nw, uint8_t *r,
    unsigned nr);
/* Stop, reset and free both channels and the event ring. */
void pandora_gpi_close(PandoraGpi *g);
#endif

#endif

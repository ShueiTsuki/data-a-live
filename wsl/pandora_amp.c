/*
 * PANDORA speaker amplifiers: Cirrus CS35L43 at 0x40 ("B", bottom) and 0x42 ("T", top) on
 * QUPv3 wrapper 3 SE0 I2C (0x01980000, Android i2c-13). Probe only: release the amps from
 * reset (GPIO 10 / 54 high, as the DT's cirrus_reset_default pins), read DEVID (0x0) and
 * REVID (0x4) of each over I2C, print. Audio itself goes over TDM0 (GPIO 122-125,
 * LPASS I2S0) from the audio DSP; nothing here plays sound.
 * SDA GPIO 64, SCL GPIO 65 (function 1, qup3_se0, pull-up). GCC (gcc-kaanapali.c):
 * wrapper 3 votes 0x52018 bits 8-12, SE0 RCG 0xa8014. I2C FIFO transfers as
 * bsd/dev/sweet_touch.c (Linux i2c-qcom-geni.c), 100 kHz from XO.
 * Boot-arg pandora_amp=1 turns it on (default off).
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

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
void pandora_amp_init(void);

#define PA_GCC_PA   0x00100000u
#define PA_SE_PA    0x01980000u
#define PA_TLMM_PA  0x0f100000u
#define PA_GPIO(n)  ((uint32_t)(n) * 0x1000u)
#define PA_VOTE     0x52018u
#define PA_VOTE_BITS (0x1fu << 8)
#define PA_RCG      0xa8014u
#define SE_STATUS   0x040u
#define SE_MCLK     0x048u
#define SE_IF_DIS   0x064u
#define SE_FW       0x068u
#define SE_CLK_SEL  0x07cu
#define SE_DMA      0x258u
#define SE_TX_LEN   0x26cu
#define SE_RX_LEN   0x270u
#define SE_SCL      0x278u
#define SE_CMD      0x600u
#define SE_CTRL     0x604u
#define SE_IRQ      0x610u
#define SE_IRQ_EN   0x614u
#define SE_IRQ_CLR  0x618u
#define SE_TX       0x700u
#define SE_RX       0x780u
#define SE_TX_COUNT 0x800u
#define SE_RX_COUNT 0x804u
#define SE_TX_WM    0x80cu
#define SE_HW_RX    0xe28u
#define SE_DONE     (1u << 0)
#define SE_NACK     (1u << 10)
#define SE_ERRORS   (0x3eu | (1u << 9) | (1u << 10) | (1u << 12) | (1u << 13) | \
                     (3u << 24) | (3u << 28))
#define SE_TX_WM_IRQ (1u << 30)
#define SE_EVENTS   (SE_DONE | SE_ERRORS | (1u << 26) | (1u << 27) | SE_TX_WM_IRQ)
#define SE_PACK0    0x0007f8feu
#define SE_PACK1    0x000ffefeu

static vm_offset_t pa_gcc, pa_se, pa_tlmm;
static int pa_chan;

static uint32_t
pa_rd(vm_offset_t b, uint32_t o)
{
	uint32_t v = *(volatile uint32_t *)(b + o);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
pa_wr(vm_offset_t b, uint32_t o, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(b + o) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
pa_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&pa_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
pa_gpio(unsigned n, unsigned func, unsigned pull, unsigned ma, int out)
{
	if (out >= 0) {
		pa_wr(pa_tlmm, PA_GPIO(n) + 4, out ? 2u : 0u);
	}
	uint32_t ctl = pa_rd(pa_tlmm, PA_GPIO(n)) & ~0x3ffu;
	ctl |= pull | (func << 2) | ((ma / 2 - 1) << 6) | (out >= 0 ? 0x200u : 0u);
	if (ctl & (1u << 11)) {
		ctl |= 1u << 12;
	}
	pa_wr(pa_tlmm, PA_GPIO(n), ctl);
}

static bool
pa_on(uint32_t cbcr)
{
	return !(pa_rd(pa_gcc, cbcr) & 0x80000000u);
}

static int
pa_clocks(unsigned *level)
{
	pa_wr(pa_gcc, PA_VOTE, pa_rd(pa_gcc, PA_VOTE) | PA_VOTE_BITS);
	static const uint32_t br[] = {0x233f0, 0x233f4, 0x233f8, 0x2340c};
	for (unsigned i = 0; i < 4; i++) {
		for (unsigned t = 0; t < 10000 && !pa_on(br[i]); t++) {
			delay(1);
		}
		if (!pa_on(br[i])) {
			printf("PANDORA AMP: wrapper 3 clock %#x off (%#x)\n", br[i], pa_rd(pa_gcc, br[i]));
			return EIO;
		}
	}
	*level = 0;
	if (pa_rd(pa_gcc, PA_RCG + 0x14) & 1u) {
		while (*level < 8 && (pa_rd(pa_gcc, PA_RCG + 0x1c + 4 * *level) & 0x371fu) != 1u) {
			(*level)++;
		}
		if (*level == 8) {
			printf("PANDORA AMP: no XO DFS level\n");
			return EIO;
		}
	} else {
		pa_wr(pa_gcc, PA_RCG + 4, (pa_rd(pa_gcc, PA_RCG + 4) & ~(0x371fu | (1u << 20))) | 1u);
		pa_wr(pa_gcc, PA_RCG, pa_rd(pa_gcc, PA_RCG) | 1u);
		for (unsigned t = 0; t < 5000 && (pa_rd(pa_gcc, PA_RCG) & 1u); t++) {
			delay(1);
		}
	}
	return 0;
}

static void
pa_abort(void)
{
	pa_wr(pa_se, SE_CTRL, 2);
	for (unsigned i = 0; i < 5000 && !(pa_rd(pa_se, SE_IRQ) & (1u << 5)); i++) {
		delay(1);
	}
	for (unsigned i = 0; i < 64 && (pa_rd(pa_se, SE_RX_COUNT) & 0x1ffffffu); i++) {
		(void)pa_rd(pa_se, SE_RX);
	}
	pa_wr(pa_se, SE_IRQ_CLR, 0xffffffffu);
}

/* One I2C message: write up to 4 bytes or read up to 8. stretch: no STOP (repeated start). */
static int
pa_msg(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr, bool stretch)
{
	if (nt > 4 || nr > 8 || (nt && nr) || (!nt && !nr)) {
		return EINVAL;
	}
	for (unsigned i = 0; i < 5000 && (pa_rd(pa_se, SE_STATUS) & 1u); i++) {
		delay(1);
	}
	if (pa_rd(pa_se, SE_STATUS) & 1u) {
		return EBUSY;
	}
	pa_wr(pa_se, SE_IRQ_CLR, 0xffffffffu);
	pa_wr(pa_se, SE_TX_WM, nt ? 1u : 0u);
	pa_wr(pa_se, nr ? SE_RX_LEN : SE_TX_LEN, nr ? nr : nt);
	pa_wr(pa_se, SE_CMD, ((nr ? 2u : 1u) << 27) | ((uint32_t)addr << 9) | (stretch ? 4u : 0u));
	unsigned got = 0;
	bool sent = nt == 0;
	for (unsigned i = 0; i < 20000; i++) {
		uint32_t irq = pa_rd(pa_se, SE_IRQ);
		if (irq & SE_ERRORS) {
			int err = (irq & SE_NACK) ? ENXIO : EIO;
			for (unsigned j = 0; j < 5000 && !(irq & SE_DONE); j++) {
				delay(1);
				irq = pa_rd(pa_se, SE_IRQ);
			}
			if (!(irq & SE_DONE) || (pa_rd(pa_se, SE_STATUS) & 1u)) {
				pa_abort();
			}
			pa_wr(pa_se, SE_IRQ_CLR, 0xffffffffu);
			return err;
		}
		if (!sent && (irq & SE_TX_WM_IRQ)) {
			uint32_t w = 0;
			for (unsigned j = 0; j < nt; j++) {
				w |= (uint32_t)tx[j] << (8u * j);
			}
			pa_wr(pa_se, SE_TX, w);
			pa_wr(pa_se, SE_TX_WM, 0);
			sent = true;
		}
		uint32_t wc = pa_rd(pa_se, SE_RX_COUNT) & 0x1ffffffu;
		while (wc--) {
			uint32_t w = pa_rd(pa_se, SE_RX);
			for (unsigned j = 0; j < 4 && got < nr; j++) {
				rx[got++] = (uint8_t)(w >> (8u * j));
			}
		}
		if (irq & SE_DONE) {
			pa_wr(pa_se, SE_IRQ_CLR, 0xffffffffu);
			return (got == nr && sent) ? 0 : EIO;
		}
		pa_wr(pa_se, SE_IRQ_CLR, irq & ((1u << 26) | (1u << 27) | SE_TX_WM_IRQ));
		delay(1);
	}
	pa_abort();
	return ETIMEDOUT;
}

/* CS35L43 register read: 32-bit address, 32-bit value, both big endian. */
static int
pa_reg(uint8_t addr, uint32_t reg, uint32_t *val)
{
	uint8_t a[4] = {(uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg};
	uint8_t v[4] = {0, 0, 0, 0};
	int err = pa_msg(addr, a, 4, NULL, 0, true);
	if (!err) {
		err = pa_msg(addr, NULL, 0, v, 4, false);
	}
	*val = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3];
	return err;
}

static void
pa_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	pa_sleep(8000);
	pa_gcc = ml_io_map(PA_GCC_PA, 0x1f0000);
	pa_se = ml_io_map(PA_SE_PA, 0x4000);
	pa_tlmm = ml_io_map(PA_TLMM_PA, 0xa0000);
	if (!pa_gcc || !pa_se || !pa_tlmm) {
		goto out;
	}
	printf("PANDORA AMP: pins before: 10=%#x/%u 54=%#x/%u 64=%#x 65=%#x vote=%#x\n",
	    pa_rd(pa_tlmm, PA_GPIO(10)), pa_rd(pa_tlmm, PA_GPIO(10) + 4) & 3u,
	    pa_rd(pa_tlmm, PA_GPIO(54)), pa_rd(pa_tlmm, PA_GPIO(54) + 4) & 3u,
	    pa_rd(pa_tlmm, PA_GPIO(64)), pa_rd(pa_tlmm, PA_GPIO(65)), pa_rd(pa_gcc, PA_VOTE));
	unsigned level = 0;
	if (pa_clocks(&level)) {
		goto out;
	}
	uint32_t fw = pa_rd(pa_se, SE_FW), rxp = pa_rd(pa_se, SE_HW_RX);
	printf("PANDORA AMP: SE fw=%#x if_disable=%#x status=%#x hw_rx=%#x level=%u\n", fw,
	    pa_rd(pa_se, SE_IF_DIS), pa_rd(pa_se, SE_STATUS), rxp, level);
	if (((fw >> 8) & 0xff) != 3 || (pa_rd(pa_se, SE_IF_DIS) & 1u)) {
		printf("PANDORA AMP: SE0 is not FIFO I2C\n");
		goto out;
	}
	pa_wr(pa_se, 0xe18, 0);
	pa_wr(pa_se, 0xe1c, 0);
	pa_wr(pa_se, 0x644, 0);
	pa_wr(pa_se, SE_IRQ_CLR, 0xffffffffu);
	pa_wr(pa_se, 0x648, 0xffffffffu);
	pa_wr(pa_se, SE_DMA, 0);
	pa_wr(pa_se, 0x28, pa_rd(pa_se, 0x28) | 0x7fu);
	pa_wr(pa_se, 0xe30, pa_rd(pa_se, 0xe30) | 0x0fu);
	pa_wr(pa_se, 0x24, 0x7f);
	pa_wr(pa_se, 0x20, 1);
	pa_wr(pa_se, SE_CLK_SEL, level);
	pa_wr(pa_se, SE_MCLK, (7u << 4) | 1u);
	pa_wr(pa_se, SE_SCL, (10u << 20) | (12u << 10) | 26u);    /* 100 kHz from 19.2 MHz */
	pa_wr(pa_se, 0x254, 0);
	pa_wr(pa_se, 0x260, SE_PACK0);
	pa_wr(pa_se, 0x264, SE_PACK1);
	pa_wr(pa_se, 0x284, SE_PACK0);
	pa_wr(pa_se, 0x288, SE_PACK1);
	pa_wr(pa_se, SE_TX_WM, 0);
	pa_wr(pa_se, 0x810, 0);
	pa_wr(pa_se, 0x814, ((rxp >> 16) & 0x3fu) > 1 ? ((rxp >> 16) & 0x3fu) - 1u : 1u);
	pa_wr(pa_se, SE_IRQ_EN, SE_EVENTS);
	for (unsigned t = 0; t < 10000 && !pa_on(0xa8004); t++) {
		delay(1);
	}
	pa_gpio(64, 1, 3, 2, -1);
	pa_gpio(65, 1, 3, 2, -1);
	pa_gpio(10, 0, 0, 16, 1);
	pa_gpio(54, 0, 0, 16, 1);
	pa_sleep(5);
	static const uint8_t amps[2] = {0x40, 0x42};
	for (unsigned i = 0; i < 2; i++) {
		uint32_t devid = 0, revid = 0;
		int e1 = pa_reg(amps[i], 0x0, &devid);
		int e2 = pa_reg(amps[i], 0x4, &revid);
		printf("PANDORA AMP: %#x (%s): DEVID err=%d %#x REVID err=%d %#x\n", amps[i],
		    i ? "top" : "bottom", e1, devid, e2, revid);
	}
out:
	printf("PANDORA AMP: done\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
pandora_amp_init(void)
{
	thread_t thread;
	int on = 0;
	(void)PE_parse_boot_argn("pandora_amp", &on, sizeof(on));
	if (!on) {
		return;
	}
	if (kernel_thread_start(pa_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */

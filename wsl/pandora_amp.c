/*
 * PANDORA speaker amplifiers: Cirrus CS35L43 at 0x40 ("B", bottom) and 0x42 ("T", top) on
 * QUPv3 wrapper 3 SE0 I2C (0x01980000, Android i2c-13). Releases the amps from reset (GPIO
 * 10 / 54 high, as the DT's cirrus_reset_default pins) and carries the I2C transfers the
 * amplifier driver will need: block reads and writes of the CS35L43's 32-bit registers (the
 * address and the values big endian, the address stepping by 4 per value). The probe reads DEVID
 * (0x0) and REVID (0x4), a block of the serial-port registers, writes the three interrupt masks
 * (all masked; nothing listens to the amps' interrupt lines) and reads them back. Audio itself goes
 * over TDM0 (GPIO 122-125, LPASS I2S0) from the audio DSP; nothing here plays sound.
 * pandora_amp=2 first resets both amps and stops the DSP's ROM program as cs35l43_probe and
 * cs35l43_dsp_init do, then writes each amplifier's DSP firmware, its coefficients and the music scene
 * from the pinned pack (tools/gpu_test.sh --amp-fw; files as Android's /odm/firmware), turned into
 * register writes by pandora_csfw.c, and reads back what decides whether they landed: the
 * algorithm list at XM word 0, every unpacked XM coefficient block, the start of PM.
 * pandora_amp=3 also does what Android does around that at boot (cs35l43_dlkm.ko, read in
 * D:\DeepSeekHarness\ampfwcheck\VERIFY.md): the register patch and the DSP's sense inputs before the
 * load, then starts the Halo core (memory ready, MPU opened, core out of reset) and checks it runs:
 * core control and MPU lock as Android's idle dump, the firmware's HALO_STATE and its heartbeat
 * counting. The amplifier itself stays powered down (no sound) and no calibration is written yet.
 * SDA GPIO 64, SCL GPIO 65 (function 1, qup3_se0, pull-up). GCC (gcc-kaanapali.c):
 * wrapper 3 votes 0x52018 bits 8-12, SE0 RCG 0xa8014. I2C FIFO transfers as
 * bsd/dev/sweet_touch.c (Linux i2c-qcom-geni.c), 100 kHz from XO.
 * Boot-arg pandora_amp=1 turns it on (default off).
 * pandora_gauge=1 looks at the fuel gauge's bus (wrapper 1 SE5, 0xa94000) without using it: who
 * has its clocks on, its pins, the SE's firmware, mode and state over 200 ms. Nothing is sent.
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
#include <kern/kalloc.h>
#include "pandora_csfw.h"

extern kern_return_t thread_terminate(thread_t);
extern int pandora_fw_read(uint64_t off, void *dst, uint64_t len);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
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
#define SE_HW_TX    0xe24u
#define SE_HW_RX    0xe28u
#define PA_MAX      128u    /* bytes in one I2C message here: the 32-word FIFOs */
#define SE_DONE     (1u << 0)
#define SE_NACK     (1u << 10)
#define SE_ERRORS   (0x3eu | (1u << 9) | (1u << 10) | (1u << 12) | (1u << 13) | \
                     (3u << 24) | (3u << 28))
#define SE_TX_WM_IRQ (1u << 30)
#define SE_EVENTS   (SE_DONE | SE_ERRORS | (1u << 26) | (1u << 27) | SE_TX_WM_IRQ)
#define SE_PACK0    0x0007f8feu
#define SE_PACK1    0x000ffefeu
/* The CS35L43's Halo DSP (cs_dsp base 0x2b80000): core control (HALO_CCM_CORE_CONTROL), MPU. */
#define PA_HALO_CORE 0x2bc1000u
#define PA_HALO_MPU  0x2bc3000u
#define PA_MPU_LOCK  (PA_HALO_MPU + 0x140u)
#define PA_HALO_RST  0x2b80010u    /* HALO_CORE_SOFT_RESET */
#define PA_HALO_WDT  0x2bc7000u    /* HALO_WDT_CONTROL */
/* The gauge's bus: wrapper 1 SE5 and its GCC votes (gcc-kaanapali.c; battcheck VERIFY.md). */
#define PG_SE_PA     0x00a94000u
#define PG_VOTE      0x52008u
#define PG_VOTE_BITS ((0xfu << 18) | (1u << 27))   /* wrapper 1 core (18, 19), m_axi (20), s_ahb (21); SE5 (27) */

static vm_offset_t pa_gcc, pa_se, pa_tlmm;
static int pa_chan;
static int pa_level;
static int pa_gauge_level;
static unsigned pa_txw = 1;   /* TX FIFO words per watermark interrupt (depth - 1, as i2c-qcom-geni) */

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

/*
 * One I2C message: a write of a header (up to 4 bytes) followed by a body of any length, fed to the
 * TX FIFO as it drains, or a read of up to PA_MAX bytes. stretch: no STOP (repeated start). The
 * deadline follows the length: 20 ms plus 200 us a byte (100 kHz moves a byte in 90 us).
 */
static int
pa_xfer(uint8_t addr, const uint8_t *hdr, unsigned nh, const uint8_t *body, unsigned nb, uint8_t *rx, unsigned nr,
    bool stretch)
{
	const unsigned nt = nh + nb;
	if (nh > 4 || nt > (1u << 20) || nr > PA_MAX || (nt && nr) || (!nt && !nr)) {
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
	unsigned got = 0, put = 0;
	uint64_t deadline;
	clock_interval_to_deadline(20000u + 200u * (nt + nr), NSEC_PER_USEC, &deadline);
	while (mach_absolute_time() < deadline) {
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
		if (put < nt && (irq & SE_TX_WM_IRQ)) {
			/* Bytes go LSB first in each FIFO word (the packing set up in pa_thread). */
			for (unsigned k = 0; k < pa_txw && put < nt; k++) {
				uint32_t w = 0;
				for (unsigned j = 0; j < 4 && put < nt; j++, put++) {
					w |= (uint32_t)(put < nh ? hdr[put] : body[put - nh]) << (8u * j);
				}
				pa_wr(pa_se, SE_TX, w);
			}
			if (put == nt) {
				pa_wr(pa_se, SE_TX_WM, 0);
			}
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
			return (got == nr && put == nt) ? 0 : EIO;
		}
		pa_wr(pa_se, SE_IRQ_CLR, irq & ((1u << 26) | (1u << 27) | SE_TX_WM_IRQ));
		delay(1);
	}
	pa_abort();
	return ETIMEDOUT;
}

static int
pa_msg(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr, bool stretch)
{
	return pa_xfer(addr, tx, nt > 4 ? 4 : nt, nt > 4 ? tx + 4 : NULL, nt > 4 ? nt - 4 : 0, rx, nr, stretch);
}

static void
pa_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* CS35L43 registers reg, reg + 4, ...: the 32-bit address, then the values (all big endian). */
static int
pa_read_regs(uint8_t addr, uint32_t reg, uint32_t *val, unsigned n)
{
	uint8_t a[4], v[PA_MAX];
	if (!n || n * 4 > PA_MAX) {
		return EINVAL;
	}
	pa_be32(a, reg);
	bzero(v, sizeof(v));
	int err = pa_msg(addr, a, 4, NULL, 0, true);
	if (!err) {
		err = pa_msg(addr, NULL, 0, v, n * 4, false);
	}
	for (unsigned i = 0; i < n; i++) {
		val[i] = ((uint32_t)v[4 * i] << 24) | ((uint32_t)v[4 * i + 1] << 16) | ((uint32_t)v[4 * i + 2] << 8) | v[4 * i + 3];
	}
	return err;
}

static int
pa_write_regs(uint8_t addr, uint32_t reg, const uint32_t *val, unsigned n)
{
	uint8_t b[PA_MAX];
	if (!n || 4 + n * 4 > PA_MAX) {
		return EINVAL;
	}
	pa_be32(b, reg);
	for (unsigned i = 0; i < n; i++) {
		pa_be32(b + 4 + 4 * i, val[i]);
	}
	return pa_msg(addr, b, 4 + n * 4, NULL, 0, false);
}

static int
pa_reg(uint8_t addr, uint32_t reg, uint32_t *val)
{
	return pa_read_regs(addr, reg, val, 1);
}

static int
pa_write1(uint8_t addr, uint32_t reg, uint32_t val)
{
	return pa_write_regs(addr, reg, &val, 1);
}

/* BYTES (whole registers, big endian as in the DSP files) to the registers from REG on, in one message. */
static int
pa_write_block(uint8_t addr, uint32_t reg, const uint8_t *data, unsigned bytes)
{
	uint8_t a[4];
	pa_be32(a, reg);
	return pa_xfer(addr, a, 4, data, bytes, NULL, 0, false);
}

/* The registers from REG on compared with BYTES at DATA; returns the number of differing registers. */
static unsigned
pa_compare(uint8_t addr, uint32_t reg, const uint8_t *data, unsigned bytes, int *err)
{
	unsigned bad = 0;
	for (unsigned at = 0; at < bytes && !*err; at += PA_MAX) {
		uint32_t v[PA_MAX / 4];
		const unsigned n = (bytes - at > PA_MAX ? PA_MAX : bytes - at) / 4;
		*err = pa_read_regs(addr, reg + at, v, n);
		for (unsigned i = 0; i < n && !*err; i++) {
			const uint8_t *d = data + at + 4 * i;
			bad += v[i] != ((uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3]);
		}
	}
	return bad;
}

/* A file from the pinned firmware pack (tools/mkfwpack.py) into a new kalloc_data buffer. */
static uint8_t *
pa_fw_file(const char *name, uint32_t *size)
{
	static uint8_t dir[4096];
	if (pandora_fw_read(0, dir, sizeof(dir)) || memcmp(dir, "PANDFW01", 8)) {
		printf("PANDORA AMP: no firmware pack\n");
		return NULL;
	}
	uint32_t n;
	memcpy(&n, dir + 8, 4);
	for (uint32_t i = 0; i < n && i < 63; i++) {
		char ename[48];
		uint64_t off, len;
		memcpy(ename, dir + 16 + 64 * i, 48);
		memcpy(&off, dir + 16 + 64 * i + 48, 8);
		memcpy(&len, dir + 16 + 64 * i + 56, 8);
		ename[47] = 0;
		if (strcmp(ename, name) || len == 0 || len > (1u << 20)) {
			continue;
		}
		uint8_t *buf = kalloc_data((vm_size_t)len, Z_WAITOK);
		if (!buf || pandora_fw_read(off, buf, len)) {
			printf("PANDORA AMP: %s: read failed\n", name);
			if (buf) {
				kfree_data(buf, (vm_size_t)len);
			}
			return NULL;
		}
		*size = (uint32_t)len;
		return buf;
	}
	printf("PANDORA AMP: %s not in the firmware pack\n", name);
	return NULL;
}

/* Every write of one file; with check, unpacked XM blocks are read back. Returns 0 or an errno. */
static int
pa_write_file(uint8_t addr, const char *what, const uint8_t *file, uint32_t size, bool coeff, const CsfwInfo *info,
    bool check)
{
	CsfwIter it;
	CsfwWrite w;
	int e = coeff ? csfw_coeff_begin(&it, file, size) : csfw_wmfw_begin(&it, file, size);
	unsigned writes = 0, bytes = 0, checked = 0, bad = 0;
	int err = 0;
	uint64_t t0 = mach_absolute_time(), ns;
	while (e == CsfwOk && !err && (e = csfw_next(&it, info, &w)) == CsfwOk) {
		err = pa_write_block(addr, w.reg, w.data, w.bytes);
		writes++;
		bytes += w.bytes;
		if (!err && check && w.type == CSFW_XM_T) {
			bad += pa_compare(addr, w.reg, w.data, w.bytes, &err);
			checked += w.bytes / 4;
		}
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	printf("PANDORA AMP: %#x %s: %u writes, %u bytes in %llu ms, %s%s; %u XM registers read back, %u differ\n", addr,
	    what, writes, bytes, ns / 1000000, e == CsfwEnd ? "all of it" : "STOPPED", err ? " (I2C error)" : "", checked,
	    bad);
	return err ? err : (e == CsfwEnd ? 0 : EINVAL);
}

/* A firmware control's register (by its short name), or 0. */
static uint32_t
pa_ctl(const uint8_t *fw, uint32_t fw_size, const CsfwInfo *info, uint32_t alg, const char *name)
{
	CsfwCtl c;
	int e = csfw_find_ctl(fw, fw_size, info, alg, name, &c);
	if (e != CsfwOk) {
		printf("PANDORA AMP: control %#x %s not found (%d)\n", alg, name, e);
		return 0;
	}
	return c.reg;
}

/*
 * Start the loaded DSP as Android does at boot: cs35l43_dsp_preload_ev writes PWRMGT_CTL (0x2900)
 * MEM_RDY, then cs_dsp_run opens the MPU (cs_dsp_halo_configure_mpu: 23 writes, lock_regions
 * 0xffffffff from cs35l43_dsp_init) and takes the core out of reset (cs_dsp_halo_start_core: set
 * RESET | EN, then clear RESET). Android's idle dump: core control 0x81, MPU lock reads 0xbeef,
 * HALO_STATE 2, HALO_HEARTBEAT counting with no audio clock.
 */
static void
pa_start(uint8_t addr, const uint8_t *fw, uint32_t fw_size, const CsfwInfo *info)
{
	static const uint16_t group[5] = {0x0, 0x4, 0x8, 0xc, 0x14};   /* XMEM, YMEM, WINDOW, XREG, YREG access */
	int err = pa_write1(addr, 0x2900, 2);
	err = err ? err : pa_write1(addr, PA_MPU_LOCK, 0x5555);
	err = err ? err : pa_write1(addr, PA_MPU_LOCK, 0xaaaa);
	for (unsigned g = 0; g < 4 && !err; g++) {
		for (unsigned k = 0; k < 5 && !err; k++) {
			err = pa_write1(addr, PA_HALO_MPU + 0x18u * g + group[k], 0xffffffffu);
		}
	}
	err = err ? err : pa_write1(addr, PA_MPU_LOCK, 0);
	uint32_t core = 0;
	err = err ? err : pa_reg(addr, PA_HALO_CORE, &core);
	err = err ? err : pa_write1(addr, PA_HALO_CORE, core | 0x201u);
	err = err ? err : pa_write1(addr, PA_HALO_CORE, (core | 0x201u) & ~0x200u);
	if (err) {
		printf("PANDORA AMP: %#x DSP start: I2C error %d\n", addr, err);
		return;
	}
	pa_sleep(20);
	const uint32_t r_state = pa_ctl(fw, fw_size, info, info->fw_id, "HALO_STATE");
	const uint32_t r_beat = pa_ctl(fw, fw_size, info, info->fw_id, "HALO_HEARTBEAT");
	const uint32_t r_audio = pa_ctl(fw, fw_size, info, 0x5f212, "AUDIO_STATE");
	uint32_t lock = 0, mem = 0, state = 0, beat0 = 0, beat1 = 0, audio = 0;
	err = pa_reg(addr, PA_HALO_CORE, &core);
	err = err ? err : pa_reg(addr, PA_MPU_LOCK, &lock);
	err = err ? err : pa_reg(addr, 0x2900, &mem);
	err = err || !r_state ? err : pa_reg(addr, r_state, &state);
	err = err || !r_beat ? err : pa_reg(addr, r_beat, &beat0);
	pa_sleep(100);
	err = err || !r_beat ? err : pa_reg(addr, r_beat, &beat1);
	err = err || !r_audio ? err : pa_reg(addr, r_audio, &audio);
	printf("PANDORA AMP: %#x DSP started: core control %#x (Android 0x81), MPU lock %#x (0xbeef), 0x2900 %#x, "
	    "HALO_STATE %#x (2), heartbeat %#x -> %#x in 100 ms, AUDIO_STATE %#x, err %d\n", addr, core, lock, mem, state,
	    beat0, beat1, audio, err);
	if (!err && r_beat && beat0 == beat1) {
		/* spk4: the core enabled but not counting, as if unclocked. cs35l43_dsp_preload_ev forces the
		 * PLL on (0x2C04 bit 16) before Android's boot-time load when its clock hook is set. */
		uint32_t pll = 0;
		err = pa_reg(addr, 0x2c04, &pll);
		err = err ? err : pa_write1(addr, 0x2c04, pll | 0x10000u);
		pa_sleep(50);
		err = err ? err : pa_reg(addr, r_beat, &beat0);
		pa_sleep(100);
		err = err ? err : pa_reg(addr, r_beat, &beat1);
		err = err ? err : pa_reg(addr, PA_HALO_CORE, &core);
		err = err ? err : pa_reg(addr, PA_MPU_LOCK, &lock);
		err = err || !r_state ? err : pa_reg(addr, r_state, &state);
		printf("PANDORA AMP: %#x PLL forced (0x2c04 %#x -> %#x): core control %#x, MPU lock %#x, HALO_STATE %#x, "
		    "heartbeat %#x -> %#x in 100 ms, err %d\n", addr, pll, pll | 0x10000u, core, lock, state, beat0, beat1, err);
	}
}

/* The amp's ordinary registers, the ones in Android's regmap dump (speakercheck amp_*_regs_before.txt),
 * for comparing with Android's idle state on the PC. Addresses and counts only. */
static void
pa_dump(uint8_t addr)
{
	static const uint32_t runs[63][2] = {{0x0, 5}, {0x20, 1}, {0x40, 2}, {0x50, 4}, {0x100, 3}, {0x110, 1}, {0x120, 3},
	    {0x140, 3}, {0x154, 2}, {0x160, 2}, {0x174, 3}, {0x400, 32}, {0x500, 8}, {0x2004, 3}, {0x2014, 10}, {0x2040, 2},
	    {0x2084, 1}, {0x2418, 4}, {0x242c, 1}, {0x2440, 1}, {0x2900, 6}, {0x2c04, 3}, {0x2c14, 1}, {0x2d10, 1},
	    {0x2d3c, 2}, {0x2d60, 2}, {0x300c, 1}, {0x3400, 1}, {0x3800, 9}, {0x3830, 1}, {0x3850, 1}, {0x4008, 1},
	    {0x4220, 2}, {0x4300, 3}, {0x4800, 5}, {0x4820, 1}, {0x4830, 1}, {0x4840, 1}, {0x4c00, 1}, {0x4c08, 1},
	    {0x4c20, 4}, {0x4c40, 6}, {0x4c60, 2}, {0x6000, 5}, {0x6200, 4}, {0x6404, 6}, {0x6450, 1}, {0x6800, 3},
	    {0x6c04, 1}, {0x7400, 2}, {0x7424, 1}, {0x10000, 2}, {0x10010, 5}, {0x10090, 5}, {0x10110, 5}, {0x10190, 5},
	    {0x10210, 1}, {0x1021c, 1}, {0x10290, 4}, {0x10314, 1}, {0x11000, 6}, {0x12000, 3}, {0x13000, 24}};
	for (unsigned r = 0; r < 63; r++) {
		for (unsigned at = 0; at < runs[r][1]; at += 8) {
			uint32_t v[8];
			const unsigned n = runs[r][1] - at > 8 ? 8 : runs[r][1] - at;
			const uint32_t reg = runs[r][0] + 4 * at;
			if (pa_read_regs(addr, reg, v, n)) {
				printf("PANDORA AMPREG: %#x %05x: read error\n", addr, reg);
				continue;
			}
			char line[8 * 9 + 1];
			for (unsigned i = 0; i < n; i++) {
				snprintf(line + 9 * i, sizeof(line) - 9 * i, " %08x", v[i]);
			}
			printf("PANDORA AMPREG: %#x %05x:%s\n", addr, reg, line);
		}
	}
}

/* One amplifier's firmware, coefficients and music scene, checked; with pandora_amp=3 started. */
static void
pa_load(uint8_t addr, char side)
{
	static CsfwInfo info;
	char name[48];
	uint32_t fw_size = 0, c_size = 0, d_size = 0;
	uint8_t *fw = pa_fw_file("cs35l43-dsp1-spk-prot.wmfw", &fw_size);
	snprintf(name, sizeof(name), "%c-cs35l43-dsp1-spk-prot.bin", side);
	uint8_t *c = pa_fw_file(name, &c_size);
	snprintf(name, sizeof(name), "%c-cs35l43-delta-music-spk-prot.bin", side);
	uint8_t *d = pa_fw_file(name, &d_size);
	if (!fw || !c || !d) {
		goto out;
	}
	int e = csfw_wmfw_info(fw, fw_size, &info);
	if (e != CsfwOk) {
		printf("PANDORA AMP: firmware algorithm list unreadable (%d)\n", e);
		goto out;
	}
	printf("PANDORA AMP: %#x (%c): firmware %#x version %u.%u.%u, %u algorithms\n", addr, side, info.fw_id,
	    info.fw_ver >> 16, info.fw_ver >> 8 & 0xff, info.fw_ver & 0xff, info.n_algs);
	uint32_t core = 0;
	int err = pa_reg(addr, PA_HALO_CORE, &core);
	printf("PANDORA AMP: %#x DSP core control %#x (err %d)\n", addr, core, err);
	if (err) {
		goto out;
	}
	/* cs35l43_probe's register patch (regmap_register_patch) */
	static const uint32_t patch[2][2] = {{0x7404, 0x11330000}, {0x3850, 0x50000802}};
	for (unsigned i = 0; i < 2 && pa_level >= 3 && !err; i++) {
		err = pa_write1(addr, patch[i][0], patch[i][1]);
	}
	if (!err && (core & 1u)) {
		/* Out of reset the core runs its ROM (spk3: 0x81 right after the reset, the serial port at
		 * its defaults). cs35l43_dsp_init stops it before anything is written (cs_dsp_stop:
		 * cs_dsp_halo_stop_watchdog, then cs_dsp_halo_stop_core: EN cleared, soft reset set). */
		uint32_t wdt = 0, rst = 0;
		err = pa_reg(addr, PA_HALO_WDT, &wdt);
		err = err ? err : pa_write1(addr, PA_HALO_WDT, wdt & ~1u);
		err = err ? err : pa_write1(addr, PA_HALO_CORE, core & ~1u);
		err = err ? err : pa_reg(addr, PA_HALO_RST, &rst);
		err = err ? err : pa_write1(addr, PA_HALO_RST, rst | 1u);
		err = err ? err : pa_reg(addr, PA_HALO_CORE, &core);
		printf("PANDORA AMP: %#x ROM stopped: watchdog %#x, core control now %#x (err %d)\n", addr, wdt, core, err);
		if (!err && (core & 1u)) {
			goto out;
		}
	}
	if (pa_level >= 3) {
		/* cs35l43_dsp_init's DSP inputs 3-6 (the sense signals) */
		static const uint32_t in[4][2] = {{0x4c48, 0x29}, {0x4c4c, 0x19}, {0x4c50, 0x18}, {0x4c54, 0x28}};
		for (unsigned i = 0; i < 4 && !err; i++) {
			err = pa_write1(addr, in[i][0], in[i][1]);
		}
	}
	if (err) {
		printf("PANDORA AMP: %#x patch, stop and DSP inputs: I2C error %d\n", addr, err);
		goto out;
	}
	if (pa_write_file(addr, "firmware", fw, fw_size, false, &info, false)) {
		goto out;
	}
	/* What the DSP will read as its id header and algorithm list: as decoded from the file. */
	uint32_t want[10 + 6 * CSFW_MAX_ALGS + 1];
	const unsigned n = 10 + 6 * info.n_algs + 1;
	const uint32_t *h = &info.core_id;
	for (unsigned i = 0; i < 9; i++) {
		want[i] = h[i];
	}
	want[9] = info.n_algs;
	for (unsigned k = 0; k < 6 * info.n_algs; k++) {
		want[10 + k] = (&info.algs[k / 6].id)[k % 6];
	}
	want[n - 1] = 0xbedead;
	unsigned bad = 0;
	for (unsigned at = 0; at < n && !err; at += PA_MAX / 4) {
		uint32_t v[PA_MAX / 4];
		const unsigned m = n - at > PA_MAX / 4 ? PA_MAX / 4 : n - at;
		err = pa_read_regs(addr, CSFW_XM + 4 * at, v, m);
		for (unsigned i = 0; i < m && !err; i++) {
			bad += v[i] != want[at + i];
		}
	}
	printf("PANDORA AMP: %#x id header and algorithm list read back: %u words, %u differ (err %d)\n", addr, n, bad, err);
	/* The first PM instructions, as written (packed). */
	CsfwIter it;
	CsfwWrite w;
	if (!err && csfw_wmfw_begin(&it, fw, fw_size) == CsfwOk) {
		while (csfw_next(&it, &info, &w) == CsfwOk) {
			if (w.type == CSFW_PM_PACKED_T) {
				unsigned pbad = pa_compare(addr, w.reg, w.data, w.bytes < PA_MAX ? w.bytes : PA_MAX, &err);
				printf("PANDORA AMP: %#x PM at %#x: first %u bytes read back, %u registers differ (err %d)\n", addr,
				    w.reg, w.bytes < PA_MAX ? w.bytes : PA_MAX, pbad, err);
				break;
			}
		}
	}
	if (err || pa_write_file(addr, "coefficients", c, c_size, true, &info, true)) {
		goto out;
	}
	if (pa_write_file(addr, "music scene", d, d_size, true, &info, true) == 0 && pa_level >= 3) {
		pa_start(addr, fw, fw_size, &info);
		pa_dump(addr);
	}
out:
	if (fw) {
		kfree_data(fw, (vm_size_t)fw_size);
	}
	if (c) {
		kfree_data(c, (vm_size_t)c_size);
	}
	if (d) {
		kfree_data(d, (vm_size_t)d_size);
	}
}

/*
 * The fuel gauge's bus, looked at and not used. Android drives it from the apps processor
 * (i2c@a94000, qcom,shared, GSI command word 0x00300001), but the SOCCP's device tree has this SE's
 * pins and its firmware the clock name, so first: who has its clocks on, how the pins are set, and
 * what the SE says (firmware, FIFO or GSI mode, busy) over 200 ms. Our votes are taken back.
 */
static void
pa_gauge(void)
{
	static const uint32_t br[5] = {0x2315c, 0x23148, 0x23140, 0x23144, 0x18508};
	const uint32_t vote = pa_rd(pa_gcc, PG_VOTE);
	printf("PANDORA GAUGE: vote %#x; branches 2315c %#x 23148 %#x 23140 %#x 23144 %#x 18508 %#x; RCG cmd %#x cfg %#x; "
	    "GPIO52 %#x/%#x GPIO53 %#x/%#x\n", vote, pa_rd(pa_gcc, br[0]), pa_rd(pa_gcc, br[1]), pa_rd(pa_gcc, br[2]),
	    pa_rd(pa_gcc, br[3]), pa_rd(pa_gcc, br[4]), pa_rd(pa_gcc, 0x18518), pa_rd(pa_gcc, 0x1851c),
	    pa_rd(pa_tlmm, PA_GPIO(52)), pa_rd(pa_tlmm, PA_GPIO(52) + 4), pa_rd(pa_tlmm, PA_GPIO(53)),
	    pa_rd(pa_tlmm, PA_GPIO(53) + 4));
	pa_wr(pa_gcc, PG_VOTE, vote | PG_VOTE_BITS);
	bool on = true;
	for (unsigned i = 0; i < 5 && on; i++) {
		for (unsigned t = 0; t < 10000 && !pa_on(br[i]); t++) {
			delay(1);
		}
		on = pa_on(br[i]);
		if (!on) {
			printf("PANDORA GAUGE: clock %#x stays off (%#x); SE not read\n", br[i], pa_rd(pa_gcc, br[i]));
		}
	}
	vm_offset_t se = on ? ml_io_map_unmappable(PG_SE_PA, 0x4000, VM_WIMG_IO) : 0;
	if (se) {
		printf("PANDORA GAUGE: SE5 fw %#x if_disable %#x dma_mode %#x status %#x cmd %#x irq %#x irq_en %#x "
		    "tx %#x rx %#x clk_sel %#x m_clk %#x scl %#x hw_tx %#x hw_rx %#x gsi_ev %#x se_irq_en %#x\n",
		    pa_rd(se, SE_FW), pa_rd(se, SE_IF_DIS), pa_rd(se, SE_DMA), pa_rd(se, SE_STATUS), pa_rd(se, SE_CMD),
		    pa_rd(se, SE_IRQ), pa_rd(se, SE_IRQ_EN), pa_rd(se, SE_TX_COUNT), pa_rd(se, SE_RX_COUNT),
		    pa_rd(se, SE_CLK_SEL), pa_rd(se, SE_MCLK), pa_rd(se, SE_SCL), pa_rd(se, SE_HW_TX), pa_rd(se, SE_HW_RX),
		    pa_rd(se, 0xe18), pa_rd(se, 0xe1c));
		/* someone else using it shows as a running command or changing interrupt status */
		unsigned busy = 0, changed = 0;
		uint32_t irq = pa_rd(se, SE_IRQ);
		for (unsigned i = 0; i < 200; i++) {
			busy += pa_rd(se, SE_STATUS) & 1u;
			const uint32_t now = pa_rd(se, SE_IRQ);
			changed += now != irq;
			irq = now;
			pa_sleep(1);
		}
		printf("PANDORA GAUGE: 200 samples over 200 ms: command active in %u, interrupt status changed %u times\n",
		    busy, changed);
		ml_io_unmap(se, 0x4000);
	}
	pa_wr(pa_gcc, PG_VOTE, pa_rd(pa_gcc, PG_VOTE) & ~(PG_VOTE_BITS & ~vote));
	printf("PANDORA GAUGE: vote back to %#x\n", pa_rd(pa_gcc, PG_VOTE));
}

static void
pa_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	pa_sleep(8000);
	pa_gcc = ml_io_map_unmappable(PA_GCC_PA, 0x1f0000, VM_WIMG_IO);
	pa_se = ml_io_map_unmappable(PA_SE_PA, 0x4000, VM_WIMG_IO);
	pa_tlmm = ml_io_map_unmappable(PA_TLMM_PA, 0xa0000, VM_WIMG_IO);
	if (!pa_gcc || !pa_se || !pa_tlmm) {
		goto out;
	}
	if (pa_gauge_level) {
		pa_gauge();
	}
	if (!pa_level) {
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
	uint32_t fw = pa_rd(pa_se, SE_FW), rxp = pa_rd(pa_se, SE_HW_RX), txp = pa_rd(pa_se, SE_HW_TX);
	pa_txw = ((txp >> 16) & 0x3fu) > 1 ? ((txp >> 16) & 0x3fu) - 1u : 1u;
	printf("PANDORA AMP: SE fw=%#x if_disable=%#x status=%#x hw_tx=%#x hw_rx=%#x level=%u, %u TX words per refill\n", fw,
	    pa_rd(pa_se, SE_IF_DIS), pa_rd(pa_se, SE_STATUS), txp, rxp, level, pa_txw);
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
	if (pa_level >= 2) {
		/* cs35l43_probe resets them first: reset low, 2.0-2.1 ms, high, 2.0-2.1 ms. */
		pa_gpio(10, 0, 0, 16, 0);
		pa_gpio(54, 0, 0, 16, 0);
		pa_sleep(3);
	}
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
		/* A block read: the serial port, 0x4800-0x4840 (17 registers). */
		uint32_t sp[17];
		bzero(sp, sizeof(sp));
		int e3 = pa_read_regs(amps[i], 0x4800, sp, 17);
		printf("PANDORA AMP: %#x serial port err=%d: 4800 %08x 4804 %08x 4808 %08x 480c %08x 4810 %08x 4820 %08x "
		    "4830 %08x 4840 %08x\n", amps[i], e3, sp[0], sp[1], sp[2], sp[3], sp[4], sp[8], sp[12], sp[16]);
		/* A block write and its read-back: IRQ1_MASK_1..3 all masked. */
		static const uint32_t masks[3] = {0xffffffffu, 0xffffffffu, 0xffffffffu};
		uint32_t back[3] = {0, 0, 0};
		int e4 = pa_write_regs(amps[i], 0x10110, masks, 3);
		int e5 = pa_read_regs(amps[i], 0x10110, back, 3);
		printf("PANDORA AMP: %#x interrupt masks written err=%d, read err=%d: %08x %08x %08x\n", amps[i], e4, e5,
		    back[0], back[1], back[2]);
		if (pa_level >= 2 && !e1 && devid == 0x35a43) {
			pa_load(amps[i], i ? 'T' : 'B');
		}
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
	int on = 0, gauge = 0;
	(void)PE_parse_boot_argn("pandora_amp", &on, sizeof(on));
	(void)PE_parse_boot_argn("pandora_gauge", &gauge, sizeof(gauge));
	if (!on && !gauge) {
		return;
	}
	pa_level = on;
	pa_gauge_level = gauge;
	if (kernel_thread_start(pa_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */

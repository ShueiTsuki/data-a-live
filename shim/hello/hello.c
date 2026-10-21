/*
 * pandora-hello: first program for the Xiaomi 17 Pro (pandora, SM8850).
 * Started by `fastboot boot`, writes nothing to storage. It finds the framebuffer ABL
 * left on screen, prints CPU facts (exception level, 16K page support, PAC, ...), the
 * display pipe registers and its own load address, counts down 120 s and resets the
 * phone back to Android with PSCI SYSTEM_RESET.
 *
 * MMU is off: every access is Device memory, so the code is built with -mstrict-align.
 * The image is linked at 0 and loaded anywhere: no absolute pointers in data.
 */
#include <stdint.h>
#include "font.h"

uint32_t safe_rd32(uint64_t pa, uint32_t *faulted);
void fatal(void) __attribute__((noreturn));
extern uint64_t fault_esr;

#define SYSREG(r) ({ uint64_t v_; __asm__ volatile("mrs %0, " #r : "=r"(v_)); v_; })

/* Display: MDP block and pipe offsets from the live DT (qcom,mdss_mdp@9800000,
 * qcom,sde-sspp-off); splash region from /reserved-memory/splash_region. */
#define MDP          0x9800000ull
#define SPLASH       0xfc800000ull
#define SPLASH_SIZE  0x2b00000ull
static const uint32_t sspp_off[10] = {
	0x2c000, 0x35000, 0x3e000, 0x47000,                     /* VIG 0-3 */
	0x98000, 0xa1000, 0xaa000, 0xb3000, 0xbc000, 0xc5000,   /* DMA 0-5 */
};
struct pipe { uint32_t size, addr, stride, fmt, faulted; };
static struct pipe pipes[10];

static volatile uint32_t *fb;
static uint32_t fb_w, fb_h, fb_stride;   /* stride in bytes */
static int row;

#define SCALE 3
#define CW    (8 * SCALE)
#define CH    (16 * SCALE)
#define MARGIN 24

#define BG     0xff102040u
#define WHITE  0xffffffffu
#define YELLOW 0xffffff00u
#define GREEN  0xff40ff40u
#define RED    0xffff4040u

static void fill(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t c)
{
	for (uint32_t y = y0; y < y0 + h && y < fb_h; y++) {
		volatile uint32_t *line = fb + y * (fb_stride / 4);
		for (uint32_t x = x0; x < x0 + w && x < fb_w; x++)
			line[x] = c;
	}
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	uint32_t x0 = MARGIN + col * CW, y0 = MARGIN + row * CH;
	if (x0 + CW > fb_w || y0 + CH > fb_h)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++)
			fill(x0 + gx * SCALE, y0 + gy * SCALE, SCALE, SCALE,
			    (g[gy] & (0x80 >> gx)) ? fg : BG);
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static char *hex(char *p, uint64_t v, int digits)
{
	for (int i = digits - 1; i >= 0; i--) {
		unsigned d = (v >> (4 * i)) & 0xf;
		*p++ = d < 10 ? '0' + d : 'A' + d - 10;
	}
	*p = 0;
	return p;
}

static char *dec(char *p, uint64_t v)
{
	char t[21];
	int n = 0;
	do t[n++] = '0' + v % 10; while (v /= 10);
	while (n)
		*p++ = t[--n];
	*p = 0;
	return p;
}

static char *cat(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	*p = 0;
	return p;
}

/* "LABEL     text" in two colours, then next row. */
static void line(const char *label, const char *text, uint32_t fg)
{
	put_str(0, label, YELLOW);
	put_str(10, text, fg);
	row++;
}

static void hexline(const char *label, uint64_t v)
{
	char b[24];
	hex(cat(b, "0x"), v, 16);
	line(label, b, WHITE);
}

static const char *gran(uint64_t f, int is4or64)
{
	if (is4or64)                     /* TGran4 / TGran64: 0 = yes, 0xf = no */
		return f == 0xf ? "NO" : (f == 1 ? "YES(52bit)" : "YES");
	return f == 0 ? "NO" : (f == 2 ? "YES(52bit)" : "YES");   /* TGran16 */
}

static void find_framebuffer(void)
{
	int pick = -1;
	for (int i = 0; i < 10; i++) {
		uint64_t b = MDP + sspp_off[i];
		uint32_t f = 0, any = 0;
		pipes[i].size = safe_rd32(b + 0x00, &f); any |= f;
		pipes[i].addr = safe_rd32(b + 0x14, &f); any |= f;
		pipes[i].stride = safe_rd32(b + 0x24, &f); any |= f;
		pipes[i].fmt = safe_rd32(b + 0x30, &f); any |= f;
		pipes[i].faulted = any;
		if (!any && pick < 0 && pipes[i].addr >= SPLASH && pipes[i].addr < SPLASH + SPLASH_SIZE)
			pick = i;
	}
	/* Default guess when no pipe scans out of the splash region. */
	uint64_t base = SPLASH;
	fb_w = 1220; fb_h = 2656; fb_stride = 1220 * 4;
	if (pick >= 0) {
		uint32_t w = pipes[pick].size & 0xffff, h = pipes[pick].size >> 16;
		uint32_t s = pipes[pick].stride & 0xffff;
		if (w >= 64 && w <= 4096 && h >= 64 && h <= 4096 && s >= w * 4 &&
		    pipes[pick].addr + (uint64_t)s * h <= SPLASH + SPLASH_SIZE) {
			base = pipes[pick].addr;
			fb_w = w; fb_h = h; fb_stride = s;
		}
	}
	/* Never write outside the splash reservation. */
	if (base + (uint64_t)fb_stride * fb_h > SPLASH + SPLASH_SIZE)
		fb_h = (uint32_t)((SPLASH + SPLASH_SIZE - base) / fb_stride);
	fb = (volatile uint32_t *)(uintptr_t)base;
}

static void psci_reset(void) __attribute__((noreturn));
static void psci_reset(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000009;
	__asm__ volatile("smc #0" : "+r"(x0) :: "x1", "x2", "x3", "memory");
	fatal();
}

void hello_main(uint64_t fdt, uint64_t el, uint64_t load)
{
	find_framebuffer();
	fill(0, 0, fb_w, fb_h, BG);
	row = 0;
	char b[64], *p;

	line("PANDORA", "hello v1 - nothing is written to storage", GREEN);
	row++;
	p = dec(cat(b, "EL"), el);
	line("LEVEL", b, el == 1 ? WHITE : RED);
	hexline("LOADED", load);
	uint32_t f = 0;
	uint32_t magic = fdt ? safe_rd32(fdt, &f) : 0;
	p = hex(cat(b, "0x"), fdt, 12);
	p = cat(p, magic == 0xedfe0dd0u ? " FDT ok" : " NO FDT");
	line("DTB", b, magic == 0xedfe0dd0u ? WHITE : RED);
	row++;

	hexline("MIDR", SYSREG(midr_el1));
	hexline("MPIDR", SYSREG(mpidr_el1));
	uint64_t mmfr0 = SYSREG(id_aa64mmfr0_el1);
	hexline("MMFR0", mmfr0);
	p = cat(b, "16K ");
	p = cat(p, gran((mmfr0 >> 20) & 0xf, 0));
	p = cat(p, "  4K ");
	p = cat(p, gran((mmfr0 >> 28) & 0xf, 1));
	p = cat(p, "  64K ");
	p = cat(p, gran((mmfr0 >> 24) & 0xf, 1));
	line("PAGES", b, ((mmfr0 >> 20) & 0xf) ? GREEN : RED);
	static const uint8_t pa_bits[8] = {32, 36, 40, 42, 44, 48, 52, 56};
	p = dec(cat(b, "PA "), pa_bits[mmfr0 & 7]);
	cat(p, " bits");
	line("", b, WHITE);
	hexline("MMFR1", SYSREG(id_aa64mmfr1_el1));
	hexline("MMFR2", SYSREG(s3_0_c0_c7_2));
	hexline("PFR0", SYSREG(id_aa64pfr0_el1));
	hexline("PFR1", SYSREG(id_aa64pfr1_el1));
	hexline("ISAR0", SYSREG(id_aa64isar0_el1));
	uint64_t isar1 = SYSREG(id_aa64isar1_el1);
	hexline("ISAR1", isar1);
	hexline("ISAR2", SYSREG(s3_0_c0_c6_2));
	uint64_t frq = SYSREG(cntfrq_el0);
	p = dec(cat(b, ""), frq);
	cat(p, " Hz");
	line("CNTFRQ", b, WHITE);
	hexline("SCTLR1", SYSREG(sctlr_el1));
	row++;

	p = hex(cat(b, "0x"), (uintptr_t)fb, 10);
	p = dec(cat(p, " "), fb_w);
	p = dec(cat(p, "x"), fb_h);
	p = dec(cat(p, " stride "), fb_stride);
	line("FB", b, WHITE);
	for (int i = 0; i < 10; i++) {
		if (pipes[i].faulted) {
			p = cat(b, "read fault esr ");
			hex(p, fault_esr, 8);
		} else {
			p = hex(b, pipes[i].addr, 8);
			p = hex(cat(p, " "), pipes[i].stride, 8);
			p = hex(cat(p, " "), pipes[i].size, 8);
			hex(cat(p, " "), pipes[i].fmt, 8);
		}
		char lab[8];
		cat(dec(cat(lab, i < 4 ? "VIG" : "DMA"), i < 4 ? i : i - 4), "");
		line(lab, b, WHITE);
	}
	row++;

	/* Colour bars: tell the pixel format apart by eye. */
	uint32_t y = MARGIN + row * CH, w = (fb_w - 2 * MARGIN) / 4;
	fill(MARGIN, y, w, CH, 0xffff0000u);
	fill(MARGIN + w, y, w, CH, 0xff00ff00u);
	fill(MARGIN + 2 * w, y, w, CH, 0xff0000ffu);
	fill(MARGIN + 3 * w, y, w, CH, 0xffffffffu);
	row++;
	line("BARS", "should read: red green blue white", WHITE);
	row++;

	/* Countdown: if it keeps changing, the panel refreshes by itself. */
	uint64_t t0 = SYSREG(cntvct_el0);
	int crow = row;
	for (uint64_t s = 120; s > 0; s--) {
		row = crow;
		fill(MARGIN, MARGIN + row * CH, fb_w - 2 * MARGIN, CH, BG);
		p = dec(cat(b, "reboot to Android in "), s);
		cat(p, " s");
		line("TIMER", b, GREEN);
		while (SYSREG(cntvct_el0) - t0 < (120 - s + 1) * frq)
			;
	}
	psci_reset();
}

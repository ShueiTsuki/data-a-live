/*
 * pandora boot menu: lives in boot_b in place of the stock Android kernel, so the phone can
 * start iOS or Android at power-on without a PC.
 *
 * ABL loads this exactly as it loads the stock kernel: vendor_boot, init_boot (Magisk), dtbo
 * and the bootconfig still come from flash, and x0 is ABL's FDT. On a normal power-on one
 * line is drawn near the bottom of the boot logo for a few seconds: volume up starts the
 * PANDORA xnu (as shim/loader does); otherwise the logo is put back and the stock Android
 * kernel embedded behind this code is entered with the same FDT, as the stock boot_b would.
 * Charger, silent and every other boot mode go to Android at once, without the line.
 * Writes nothing to storage.
 *
 * MMU is off: every access is Device memory (-mstrict-align, byte or aligned reads).
 * Linked at 0 and loaded anywhere: no absolute pointers in data.
 */
#include <stdint.h>
#include "font.h"

void fatal(void) __attribute__((noreturn));
void jump_linux(uint64_t entry, uint64_t fdt) __attribute__((noreturn));
uint32_t safe_rd32(uint64_t pa, uint32_t *faulted);

#define FB          0xfc800000ull
#define FB_W        1220u
#define FB_H        2656u
#define FB_STRIDE   (1220u * 4)
/* Kernel RAM must stay inside the clean 0x8c0000000 + 13 GiB range (DT_FINDINGS.md). */
#define RAM_LO      0x8c0000000ull
#define RAM_HI      0xc00000000ull
#define BOOT_LINE_LENGTH 608
#define PAGE_16K    0x4000ull
#define SZ_2M       0x200000ull
/* Volume up: TLMM GPIO 101, active low (xnu patches 0128/0130; 0x0f100000 + 0x1000 * n,
 * the DT's 0x0f065000 hangs the bus). +0 config (FUNC bits 2-5, OE bit 9), +4 bit 0 input. */
#define VOLUP_GPIO  0x0f165000ull

/* ---- settings page, filled in by mkboot.py ---- */
struct bm_config {
	char magic[8];			/* "PNDBMCF1" */
	uint32_t armed;			/* 1: volume up may start iOS; the Android watchdog clears it */
	uint32_t wait_ms;		/* how long the line stays up */
	uint32_t any_mode;		/* test builds: the line in every boot mode */
	uint32_t reserved;
	uint8_t extents_sha256[3][32];	/* iosroot, ioslog, adspfw: pinfile `extents --json` */
	char why[64];			/* set by the watchdog with armed = 0 */
};

__attribute__((section(".config"), used, aligned(4096)))
volatile struct bm_config bm_config = { .magic = { 'P', 'N', 'D', 'B', 'M', 'C', 'F', '1' } };

extern const unsigned char _payload_start[], _payload_end[], _linux_start[], _linux_end[];

static uint8_t rd8(uint64_t a) { return *(volatile const uint8_t *)(uintptr_t)a; }
static uint32_t rd32(uint64_t a) { return *(volatile const uint32_t *)(uintptr_t)a; }
static uint64_t rd64(uint64_t a) { return *(volatile const uint64_t *)(uintptr_t)a; }

static uint32_t be32(uint64_t a)
{
	return (uint32_t)rd8(a) << 24 | (uint32_t)rd8(a + 1) << 16 | (uint32_t)rd8(a + 2) << 8 | rd8(a + 3);
}

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static uint64_t tick_hz(void)
{
	uint64_t f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f;
}

static void wait_ms(uint64_t ms)
{
	uint64_t t0 = ticks(), n = tick_hz() * ms / 1000;
	while (ticks() - t0 < n)
		;
}

/* ---- screen (from shim/loader) ---- */
#define SCALE  3
#define CW     (8 * SCALE)
#define CH     (16 * SCALE)
#define MARGIN 24
#define BG     0xff102040u
#define BLACK  0xff000000u
#define WHITE  0xffffffffu
#define GREY   0xff909090u
#define YELLOW 0xffffff00u
#define GREEN  0xff40ff40u
#define RED    0xffff4040u
/* The menu line: a band near the bottom of the logo, saved first and put back after. */
#define BAND_Y (FB_H - 400u)
#define BAND_H (CH + 48u)
#define BAND_BYTES ((uint64_t)FB_STRIDE * BAND_H)

static volatile uint32_t *const fb = (volatile uint32_t *)(uintptr_t)FB;
static int row;

static void fill(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t c)
{
	for (uint32_t y = y0; y < y0 + h && y < FB_H; y++) {
		volatile uint32_t *line = fb + y * (FB_STRIDE / 4);
		for (uint32_t x = x0; x < x0 + w && x < FB_W; x++)
			line[x] = c;
	}
}

static void glyph(uint32_t x0, uint32_t y0, char ch, uint32_t fg, uint32_t bg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	if (x0 + CW > FB_W || y0 + CH > FB_H)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++)
			fill(x0 + gx * SCALE, y0 + gy * SCALE, SCALE, SCALE,
			    (g[gy] & (0x80 >> gx)) ? fg : bg);
}

static uint32_t slen(const char *s)
{
	uint32_t n = 0;
	while (s[n])
		n++;
	return n;
}

static uint32_t scopy(char *d, uint32_t at, const char *s)
{
	while (*s)
		d[at++] = *s++;
	d[at] = 0;
	return at;
}

static void line(const char *label, const char *text, uint32_t fg)
{
	uint32_t y = MARGIN + row * CH, c = 0;
	while (*label)
		glyph(MARGIN + CW * c++, y, *label++, YELLOW, BG);
	c = 10;
	while (*text)
		glyph(MARGIN + CW * c++, y, *text++, fg, BG);
	row++;
}

static void hexline(const char *label, uint64_t v)
{
	char b[19];
	b[0] = '0'; b[1] = 'x';
	for (int i = 0; i < 16; i++) {
		unsigned d = (v >> (4 * (15 - i))) & 0xf;
		b[2 + i] = d < 10 ? '0' + d : 'A' + d - 10;
	}
	b[18] = 0;
	line(label, b, WHITE);
}

/* One centred line in the band, black behind it. */
static void band_text(const char *s, uint32_t fg)
{
	fill(0, BAND_Y, FB_W, BAND_H, BLACK);
	uint32_t n = slen(s), x = n * CW < FB_W ? (FB_W - n * CW) / 2 : 0;
	for (uint32_t i = 0; i < n; i++)
		glyph(x + i * CW, BAND_Y + 24, s[i], fg, BLACK);
}

static void band_copy(uint64_t save, int restore)
{
	volatile uint64_t *f = (volatile uint64_t *)(uintptr_t)(FB + (uint64_t)BAND_Y * FB_STRIDE);
	volatile uint64_t *s = (volatile uint64_t *)(uintptr_t)save;
	for (uint64_t i = 0; i < BAND_BYTES / 8; i++) {
		if (restore)
			f[i] = s[i];
		else
			s[i] = f[i];
	}
}

/* ---- ABL's FDT: /chosen properties ---- */
static int name_is(uint64_t a, const char *s)
{
	while (*s)
		if (rd8(a++) != (uint8_t)*s++)
			return 0;
	return rd8(a) == 0;
}

/* Address of /chosen/<prop>'s value and its length, or 0. */
static uint64_t chosen_prop(uint64_t fdt, const char *prop, uint32_t *len)
{
	if (fdt == 0 || (fdt & 3) || be32(fdt) != 0xd00dfeedu)
		return 0;
	uint32_t total = be32(fdt + 4), off_struct = be32(fdt + 8), off_strings = be32(fdt + 12);
	uint32_t size_struct = be32(fdt + 36);
	if (total > (16u << 20) || off_struct >= total || size_struct > total - off_struct ||
	    off_strings >= total)
		return 0;
	uint64_t p = fdt + off_struct, end = p + size_struct;
	int depth = 0, in_chosen = 0;
	while (p + 4 <= end) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == 1) {			/* FDT_BEGIN_NODE */
			depth++;
			if (depth == 2)
				in_chosen = name_is(p, "chosen");
			while (p < end && rd8(p))
				p++;
			p = (p + 4) & ~3ull;
		} else if (tok == 2) {		/* FDT_END_NODE */
			if (depth == 2)
				in_chosen = 0;
			depth--;
		} else if (tok == 3) {		/* FDT_PROP */
			uint32_t l = be32(p), nameoff = be32(p + 4);
			p += 8;
			if (l > end - p)
				return 0;
			if (in_chosen && depth == 2 && off_strings + nameoff < total &&
			    name_is(fdt + off_strings + nameoff, prop)) {
				*len = l;
				return p;
			}
			p = (p + l + 3) & ~3ull;
		} else if (tok != 4) {		/* FDT_NOP; FDT_END or garbage ends the walk */
			break;
		}
	}
	return 0;
}

/* `w` is one whole space-separated word of the string at s (n bytes). */
static int has_word(uint64_t s, uint32_t n, const char *w)
{
	uint32_t wl = slen(w);
	for (uint32_t i = 0; i + wl <= n; i++) {
		if (i && rd8(s + i - 1) != ' ')
			continue;
		uint32_t k = 0;
		while (k < wl && rd8(s + i + k) == (uint8_t)w[k])
			k++;
		if (k == wl && (i + wl == n || rd8(s + i + wl) == ' ' || rd8(s + i + wl) == 0))
			return 1;
	}
	return 0;
}

/* A plain power-on or reboot: the line is shown only then (the words ABL writes on this phone,
 * device/pandora-live.dts). Anything else, charger mode included, goes straight to Android. */
static int normal_mode(uint64_t fdt)
{
	uint32_t n = 0;
	uint64_t args = chosen_prop(fdt, "bootargs", &n);
	return args && has_word(args, n, "bootmonitor.bootmode=normal") &&
	    has_word(args, n, "mca_log.charge_boot_mode=0") &&
	    has_word(args, n, "silent_boot.mode=nonsilent");
}

static int overlaps(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
	return a < b + bn && b < a + an;
}

/* ABL put the FDT and the initrd (ramdisks + bootconfig) outside the memory this image
 * declared, so the save area and an alignment move cannot touch them. */
static int layout_ok(uint64_t fdt, uint64_t load, uint64_t declared)
{
	if (fdt == 0 || be32(fdt) != 0xd00dfeedu || overlaps(fdt, be32(fdt + 4), load, declared))
		return 0;
	uint32_t ls = 0, le = 0;
	uint64_t s = chosen_prop(fdt, "linux,initrd-start", &ls);
	uint64_t e = chosen_prop(fdt, "linux,initrd-end", &le);
	if (!s || !e || ls != 8 || le != 8)
		return 0;
	uint64_t is = (uint64_t)be32(s) << 32 | be32(s + 4), ie = (uint64_t)be32(e) << 32 | be32(e + 4);
	return ie >= is && !overlaps(is, ie - is, load, declared);
}

/* ---- volume up ---- */
static int volup_usable(void)
{
	uint32_t faulted = 0, cfg = safe_rd32(VOLUP_GPIO, &faulted);
	return !faulted && (cfg & 0x3cu) == 0 && !(cfg & 0x200u);
}

static int volup_pressed(void)
{
	uint32_t faulted = 0, io = safe_rd32(VOLUP_GPIO + 4, &faulted);
	return !faulted && !(io & 1u);
}

/* The line, until volume up (1) or the time is up (0). */
static int menu(uint64_t save)
{
	band_copy(save, 0);
	uint64_t hz = tick_hz(), t0 = ticks(), total = hz * bm_config.wait_ms / 1000;
	/* Built by hand: no string initialisers (they become memcpy calls). */
	char msg[64];
	uint32_t n = scopy(msg, 0, bm_config.armed ? "Volume up  iOS   " : "iOS off: ");
	if (!bm_config.armed) {
		for (uint32_t i = 0; i < 40 && bm_config.why[i] >= 0x20 && bm_config.why[i] < 0x7f; i++)
			msg[n++] = bm_config.why[i];
		msg[n] = 0;
		band_text(msg, GREY);
		wait_ms(bm_config.wait_ms);
		band_copy(save, 1);
		return 0;
	}
	msg[n + 1] = 0;
	uint32_t held = 0, shown = ~0u;
	for (;;) {
		uint64_t t = ticks() - t0;
		if (t >= total)
			break;
		uint32_t left = (uint32_t)((total - t + hz - 1) / hz);
		if (left != shown) {
			msg[n] = left > 9 ? '9' : '0' + left;
			band_text(msg, WHITE);
			shown = left;
		}
		held = volup_pressed() ? held + 1 : 0;
		if (held >= 3)			/* 30 ms */
			return 1;
		wait_ms(10);
	}
	band_copy(save, 1);
	return 0;
}

/* ---- iOS: as shim/loader/loader.c ---- */
struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

static void copy64(uint64_t dst, uint64_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)(uintptr_t)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)(uintptr_t)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uint64_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)(uintptr_t)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int in_ram(uint64_t a, uint64_t n)
{
	return a >= RAM_LO && n <= RAM_HI - a;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

static const char *boot_ios(void)
{
	uint64_t pl = (uintptr_t)_payload_start, pl_end = (uintptr_t)_payload_end;
	const struct payload *p = (const struct payload *)(uintptr_t)pl;
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (p->magic[i] != want[i])
			return "no SWEETXNU payload embedded";
	if (p->version != 1 || (p->adt_off & 7) || (pl & 7) || p->nsegs == 0 || p->nsegs > 64)
		return "bad payload header";
	hexline("PAYLOAD", pl);
	hexline("PHYS", p->phys_base);
	hexline("MEMSIZE", p->mem_size);
	hexline("ENTRY", p->entry_va);
	if (!in_ram(p->phys_base, p->mem_size))
		return "kernel RAM outside the clean 0x8c0000000 range";
	if (pl_end > p->phys_base && pl < p->phys_base + p->mem_size)
		return "payload overlaps kernel memory";

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;
	/* Check every destination before writing anything. */
	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		if (!in_ram(s->vmaddr + pa_of_va_base, s->vmsize) || s->file_size > s->vmsize ||
		    s->file_off + s->file_size > pl_end - pl)
			return "unsafe payload segment";
	}
	uint64_t adt_pa = p->kernel_end + pa_of_va_base;
	uint64_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uint64_t top = ba_pa + PAGE_16K;
	if (!in_ram(adt_pa, top - adt_pa) || top > p->phys_base + p->mem_size ||
	    p->adt_off + p->adt_size > pl_end - pl)
		return "ADT/boot_args outside kernel RAM";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uint64_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, pl + s->file_off, s->file_size);
		uint64_t done = (s->file_size + 7) & ~7ull;
		if (s->vmsize > done)
			zero64(dst + done, s->vmsize - done);
	}
	hexline("KERN_END", p->kernel_end + pa_of_va_base);

	copy64(adt_pa, pl + p->adt_off, p->adt_size);
	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)(uintptr_t)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* read with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	hexline("BOOTARGS", ba_pa);

	uint64_t entry_pa = p->entry_va + pa_of_va_base;
	if (!in_ram(entry_pa, 4))
		return "entry outside kernel RAM";
	hexline("JUMP", entry_pa);
	line("XNU", "jumping (the kernel console takes over)", GREEN);
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)(uintptr_t)entry_pa)(ba_pa);
	return "xnu returned";
}

/* ---- Android: the stock kernel, unchanged, with ABL's FDT ---- */
static void halt_with(const char *label, const char *err) __attribute__((noreturn));
static void halt_with(const char *label, const char *err)
{
	line(label, err, RED);
	/* Leave the message up for 60 s, then reset. */
	wait_ms(60000);
	fatal();
}

static void start_android(uint64_t fdt, uint64_t load, uint64_t declared, int layout)
{
	uint64_t src = (uintptr_t)_linux_start, n = (uintptr_t)_linux_end - src;
	if (rd32(src + 56) != 0x644d5241u)	/* "ARM\x64" */
		halt_with("ANDROID", "no arm64 kernel embedded");
	uint64_t text_offset = rd64(src + 8), size = rd64(src + 16);
	/* arm64 boot protocol: text_offset bytes above a 2 MiB aligned base. */
	uint64_t dst = ((src - text_offset + SZ_2M - 1) & ~(SZ_2M - 1)) + text_offset;
	if (dst != src) {
		if (!layout || dst + size > load + declared)
			halt_with("ANDROID", "kernel not 2 MiB aligned and no room to move it");
		/* Upwards, overlapping: copy from the end. */
		volatile uint64_t *d = (volatile uint64_t *)(uintptr_t)dst;
		volatile const uint64_t *s = (volatile const uint64_t *)(uintptr_t)src;
		for (uint64_t i = (n + 7) / 8; i-- > 0;)
			d[i] = s[i];
	}
	jump_linux(dst, fdt);
}

void menu_main(uint64_t fdt, uint64_t el, uint64_t load)
{
	uint64_t declared = rd64(load + 16);
	uint64_t linux_src = (uintptr_t)_linux_start;
	int layout = layout_ok(fdt, load, declared);
	/* Save area: behind the Android kernel's image_size and the 2 MiB move slack. */
	uint64_t save = linux_src + rd64(linux_src + 16) + SZ_2M;
	int room = layout && save + BAND_BYTES <= load + declared;
	if (room && el == 1 && (bm_config.any_mode || normal_mode(fdt)) && volup_usable() && menu(save)) {
		fill(0, 0, FB_W, FB_H, BG);
		row = 0;
		line("PANDORA", "boot menu v1 - nothing is written to storage", GREEN);
		hexline("LOADED", load);
		halt_with("ERROR", boot_ios());
	}
	start_android(fdt, load, declared, layout);
}
